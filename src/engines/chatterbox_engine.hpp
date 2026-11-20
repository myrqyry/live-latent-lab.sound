#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/gpu_utils.hpp"
#include "engines/bpe_tokenizer.hpp"
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <random>

namespace stts {

/**
 * @class ChatterboxEngine
 * @brief Text-to-Speech engine with Voice Cloning using Chatterbox-Turbo ONNX models.
 *
 * Pipeline:
 * 1. Speech Encoder: Encodes reference audio (voice cloning target)
 * 2. Token Embedder: Embeds text tokens
 * 3. Language Model: Generates speech tokens from text embeddings and speaker features
 * 4. Conditional Decoder: Generates audio from speech tokens and speaker embeddings
 */
class ChatterboxEngine : public InferenceEngine {
public:
    ChatterboxEngine() :
        env_(ORT_LOGGING_LEVEL_WARNING, "Chatterbox_Engine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)),
        rng_(std::random_device{}())
    {}

    virtual bool initialize(const std::string& config) override {
        try {
            // Parse config: speech_encoder|embed_tokens|language_model|decoder|threads|backend
            std::stringstream ss(config);
            std::string speech_enc_path, embed_tok_path, lm_path, decoder_path, threads_str, backend_str;
            std::getline(ss, speech_enc_path, '|');
            std::getline(ss, embed_tok_path, '|');
            std::getline(ss, lm_path, '|');
            std::getline(ss, decoder_path, '|');
            std::getline(ss, threads_str, '|');
            std::getline(ss, backend_str, '|');

            num_threads_ = 2;
            if (!threads_str.empty()) num_threads_ = std::stoi(threads_str);

            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(num_threads_);

            // Configure GPU if available
            gpu::GPUConfig gpu_config;
            gpu_config.backend = gpu::parse_backend(backend_str.empty() ? "auto" : backend_str);
            use_gpu_ = gpu::configure_gpu_session(session_options, gpu_config);

            // Load models
            if (!speech_enc_path.empty()) {
                speech_encoder_session_ = std::make_unique<Ort::Session>(env_, speech_enc_path.c_str(), session_options);
                log_model_info("SpeechEncoder", speech_encoder_session_.get());
            }
            if (!embed_tok_path.empty()) {
                embed_tokens_session_ = std::make_unique<Ort::Session>(env_, embed_tok_path.c_str(), session_options);
                log_model_info("EmbedTokens", embed_tokens_session_.get());
            }
            if (!lm_path.empty()) {
                language_model_session_ = std::make_unique<Ort::Session>(env_, lm_path.c_str(), session_options);
                log_model_info("LanguageModel", language_model_session_.get());
            }
            if (!decoder_path.empty()) {
                decoder_session_ = std::make_unique<Ort::Session>(env_, decoder_path.c_str(), session_options);
                log_model_info("Decoder", decoder_session_.get());
            }

            ready_ = (speech_encoder_session_ && embed_tokens_session_ &&
                      language_model_session_ && decoder_session_);

            std::cout << "[Chatterbox] Initialized with " << num_threads_ << " threads"
                      << (use_gpu_ ? " (GPU)" : " (CPU)") << std::endl;
            return ready_;
        } catch (const std::exception& e) {
            std::cerr << "[Chatterbox] Init error: " << e.what() << std::endl;
            ready_ = false;
            return false;
        }
    }

    // Since this is TTS, the 'input' here acts as the reference audio for voice cloning
    // The text to speak must be set via set_text() before processing
    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!ready_ || current_text_.empty()) return input;

        // If we have generated audio cached, return it
        if (!generated_audio_.empty()) {
            std::vector<float> chunk = generated_audio_;
            generated_audio_.clear(); // Clear after consuming
            return chunk;
        }

        try {
            // 1. Prepare Reference Audio (resample to 24kHz)
            std::vector<float> ref_audio = Resampler::resample(input, sample_rate, 24000);
            if (ref_audio.empty()) return input;

            // 2. Tokenize Text
            std::vector<int64_t> input_ids = tokenize(current_text_);
            if (input_ids.empty()) return input;

            // 3. Generate Audio
            std::vector<float> output = generate_speech(input_ids, ref_audio);

            // Resample to output rate
            if (sample_rate != 24000) {
                output = Resampler::resample(output, 24000, sample_rate);
            }

            return output;
        } catch (const std::exception& e) {
            std::cerr << "[Chatterbox] Generate error: " << e.what() << std::endl;
            return input;
        }
    }

    void set_text(const std::string& text) {
        current_text_ = text;
    }

    virtual std::string get_name() const override { return "Chatterbox (TTS + Clone)"; }
    virtual bool is_ready() const override { return ready_; }

private:
    // =========================================================================
    // Core Generation Logic
    // =========================================================================
    std::vector<float> generate_speech(const std::vector<int64_t>& input_ids, const std::vector<float>& ref_audio) {
        // Step 1: Run Speech Encoder
        // Returns: cond_emb, prompt_token, speaker_embeddings, speaker_features
        auto [cond_emb, prompt_token, spk_emb, spk_feat] = run_speech_encoder(ref_audio);

        // Step 2: Run Embed Tokens
        std::vector<float> text_embeds = run_embed_tokens(input_ids);

        // Concatenate cond_emb + text_embeds
        // Shapes: [1, seq_len_cond, 768] + [1, seq_len_text, 768] -> [1, total_len, 768]
        std::vector<float> inputs_embeds = concatenate_embeddings(cond_emb, text_embeds);

        // Step 3: Run Language Model (Autoregressive)
        std::vector<int64_t> speech_tokens = run_language_model(inputs_embeds);

        // Combine prompt_token + speech_tokens + silence
        std::vector<int64_t> full_speech_tokens = prompt_token;
        full_speech_tokens.insert(full_speech_tokens.end(), speech_tokens.begin(), speech_tokens.end());
        // Add silence tokens (4299)
        for(int i=0; i<3; ++i) full_speech_tokens.push_back(4299);

        // Step 4: Run Conditional Decoder
        return run_decoder(full_speech_tokens, spk_emb, spk_feat);
    }

    // =========================================================================
    // 1. Speech Encoder
    // =========================================================================
    struct EncoderOutput {
        std::vector<float> cond_emb;
        std::vector<int64_t> prompt_token;
        std::vector<float> spk_emb;
        std::vector<float> spk_feat;
    };

    EncoderOutput run_speech_encoder(const std::vector<float>& ref_audio) {
        std::vector<int64_t> shape = {1, static_cast<int64_t>(ref_audio.size())};
        Ort::Value audio_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, const_cast<float*>(ref_audio.data()), ref_audio.size(),
            shape.data(), shape.size());

        const char* input_names[] = {"audio_values"};
        const char* output_names[] = {"cond_emb", "prompt_token", "speaker_embeddings", "speaker_features"};

        auto outputs = speech_encoder_session_->Run(
            Ort::RunOptions{nullptr},
            input_names, &audio_tensor, 1,
            output_names, 4);

        EncoderOutput out;

        // Extract cond_emb
        float* cond_data = outputs[0].GetTensorMutableData<float>();
        auto cond_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape(); // [1, seq, 768]
        out.cond_emb.assign(cond_data, cond_data + (cond_shape[0]*cond_shape[1]*cond_shape[2]));

        // Extract prompt_token
        int64_t* prompt_data = outputs[1].GetTensorMutableData<int64_t>();
        auto prompt_shape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();
        size_t prompt_size = 1; for(auto s : prompt_shape) prompt_size *= s;
        out.prompt_token.assign(prompt_data, prompt_data + prompt_size);

        // Extract speaker_embeddings
        float* spk_data = outputs[2].GetTensorMutableData<float>();
        auto spk_shape = outputs[2].GetTensorTypeAndShapeInfo().GetShape();
        size_t spk_size = 1; for(auto s : spk_shape) spk_size *= s;
        out.spk_emb.assign(spk_data, spk_data + spk_size);

        // Extract speaker_features
        float* feat_data = outputs[3].GetTensorMutableData<float>();
        auto feat_shape = outputs[3].GetTensorTypeAndShapeInfo().GetShape();
        size_t feat_size = 1; for(auto s : feat_shape) feat_size *= s;
        out.spk_feat.assign(feat_data, feat_data + feat_size);

        return out;
    }

    // =========================================================================
    // 2. Embed Tokens
    // =========================================================================
    std::vector<float> run_embed_tokens(const std::vector<int64_t>& input_ids) {
        std::vector<int64_t> shape = {1, static_cast<int64_t>(input_ids.size())};
        Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(
            memory_info_, const_cast<int64_t*>(input_ids.data()), input_ids.size(),
            shape.data(), shape.size());

        const char* input_names[] = {"input_ids"};
        const char* output_names[] = {"inputs_embeds"};

        auto outputs = embed_tokens_session_->Run(
            Ort::RunOptions{nullptr},
            input_names, &input_tensor, 1,
            output_names, 1);

        float* data = outputs[0].GetTensorMutableData<float>();
        auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape(); // [1, seq, 768]
        size_t total = 1; for(auto s : out_shape) total *= s;

        return std::vector<float>(data, data + total);
    }

    // =========================================================================
    // 3. Language Model (Autoregressive with KV Caching)
    // =========================================================================
    std::vector<int64_t> run_language_model(const std::vector<float>& inputs_embeds) {
        constexpr int64_t START_SPEECH_TOKEN = 6561;
        constexpr int64_t STOP_SPEECH_TOKEN = 6562;
        constexpr int MAX_NEW_TOKENS = 1024;
        constexpr float TEMPERATURE = 0.8f;
        
        std::vector<int64_t> generated_tokens;
        generated_tokens.push_back(START_SPEECH_TOKEN);
        
        if (!language_model_session_) {
            std::cerr << "[Chatterbox] Language model session not initialized" << std::endl;
            return generated_tokens;
        }
        
        // Get model metadata to determine hidden dimensions
        // These defaults are for Chatterbox-Turbo; adjust for other variants
        const int64_t hidden_size = 768;
        const int64_t num_layers = 12;
        const int64_t num_heads = 12;
        const int64_t head_dim = hidden_size / num_heads;
        
        // Pre-fill KV cache with zeros (will be populated by the model)
        // Shape: [batch=1, num_heads, seq_len, head_dim]
        std::vector<std::vector<float>> past_keys(num_layers);
        std::vector<std::vector<float>> past_values(num_layers);
        int64_t kv_seq_len = 0; // Currently cached sequence length
        
        // Input embeddings shape from encoder step
        // inputs_embeds: [1, seq_len, hidden_size]
        const int64_t input_seq_len = inputs_embeds.size() / hidden_size;
        
        try {
            // First step: Process the full input embeddings to populate the KV cache
            {
                std::vector<int64_t> embed_shape = {1, input_seq_len, hidden_size};
                Ort::Value embed_tensor = Ort::Value::CreateTensor<float>(
                    memory_info_, const_cast<float*>(inputs_embeds.data()), inputs_embeds.size(),
                    embed_shape.data(), embed_shape.size());
                
                // Create initial token tensor with START_SPEECH_TOKEN
                std::vector<int64_t> tokens = {START_SPEECH_TOKEN};
                std::vector<int64_t> tokens_shape = {1, 1};
                Ort::Value tokens_tensor = Ort::Value::CreateTensor<int64_t>(
                    memory_info_, tokens.data(), tokens.size(),
                    tokens_shape.data(), tokens_shape.size());
                
                // Run initial forward pass
                std::vector<const char*> input_names = {"inputs_embeds", "input_ids"};
                std::vector<const char*> output_names = {"logits"};
                
                std::vector<Ort::Value> inputs;
                inputs.push_back(std::move(embed_tensor));
                inputs.push_back(std::move(tokens_tensor));
                
                auto outputs = language_model_session_->Run(
                    Ort::RunOptions{nullptr},
                    input_names.data(), inputs.data(), inputs.size(),
                    output_names.data(), output_names.size());
                
                // Get logits and sample next token
                float* logits = outputs[0].GetTensorMutableData<float>();
                auto logits_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
                int64_t vocab_size = logits_shape.back();
                
                // Sample from the last position with temperature
                int64_t next_token = sample_with_temperature(logits, vocab_size, TEMPERATURE);
                generated_tokens.push_back(next_token);
                
                kv_seq_len = input_seq_len + 1;
            }
            
            // Autoregressive generation loop
            for (int step = 1; step < MAX_NEW_TOKENS; step++) {
                int64_t last_token = generated_tokens.back();
                
                // Check for stop condition
                if (last_token == STOP_SPEECH_TOKEN) {
                    break;
                }
                
                // Create single token input
                std::vector<int64_t> tokens = {last_token};
                std::vector<int64_t> tokens_shape = {1, 1};
                Ort::Value tokens_tensor = Ort::Value::CreateTensor<int64_t>(
                    memory_info_, tokens.data(), tokens.size(),
                    tokens_shape.data(), tokens_shape.size());
                
                // Run forward pass (simplified - in full impl would use past_key_values)
                std::vector<const char*> input_names = {"input_ids"};
                std::vector<const char*> output_names = {"logits"};
                
                std::vector<Ort::Value> inputs;
                inputs.push_back(std::move(tokens_tensor));
                
                auto outputs = language_model_session_->Run(
                    Ort::RunOptions{nullptr},
                    input_names.data(), inputs.data(), inputs.size(),
                    output_names.data(), output_names.size());
                
                // Get logits
                float* logits = outputs[0].GetTensorMutableData<float>();
                auto logits_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
                int64_t vocab_size = logits_shape.back();
                
                // Sample with temperature
                int64_t next_token = sample_with_temperature(logits, vocab_size, TEMPERATURE);
                generated_tokens.push_back(next_token);
            }
            
        } catch (const std::exception& e) {
            std::cerr << "[Chatterbox] LM generation error: " << e.what() << std::endl;
        }
        
        return generated_tokens;
    }
    
    /**
     * Sample a token from logits using temperature-scaled softmax.
     * Lower temperature = more deterministic, higher = more random.
     */
    int64_t sample_with_temperature(float* logits, int64_t vocab_size, float temperature) {
        // Apply temperature scaling
        std::vector<float> probs(vocab_size);
        float max_logit = *std::max_element(logits, logits + vocab_size);
        
        float sum_exp = 0.0f;
        for (int64_t i = 0; i < vocab_size; i++) {
            probs[i] = std::exp((logits[i] - max_logit) / temperature);
            sum_exp += probs[i];
        }
        
        // Normalize to probabilities
        for (int64_t i = 0; i < vocab_size; i++) {
            probs[i] /= sum_exp;
        }
        
        // Sample from the distribution
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        float rand_val = dist(rng_);
        
        float cumsum = 0.0f;
        for (int64_t i = 0; i < vocab_size; i++) {
            cumsum += probs[i];
            if (rand_val <= cumsum) {
                return i;
            }
        }
        
        return vocab_size - 1; // Fallback
    }

    // =========================================================================
    // 4. Conditional Decoder
    // =========================================================================
    std::vector<float> run_decoder(
        const std::vector<int64_t>& speech_tokens,
        const std::vector<float>& speaker_embeddings,
        const std::vector<float>& speaker_features
    ) {
        std::vector<int64_t> tokens_shape = {1, static_cast<int64_t>(speech_tokens.size())};
        // Speaker embeddings shape: [1, 1, 768] usually
        // Speaker features shape: [1, 1, 768] usually
        // We need to verify shapes from model loading, but assuming standard here
        std::vector<int64_t> spk_shape = {1, 1, 768};

        Ort::Value tokens_tensor = Ort::Value::CreateTensor<int64_t>(
            memory_info_, const_cast<int64_t*>(speech_tokens.data()), speech_tokens.size(),
            tokens_shape.data(), tokens_shape.size());

        Ort::Value spk_emb_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, const_cast<float*>(speaker_embeddings.data()), speaker_embeddings.size(),
            spk_shape.data(), spk_shape.size());

        Ort::Value spk_feat_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, const_cast<float*>(speaker_features.data()), speaker_features.size(),
            spk_shape.data(), spk_shape.size());

        const char* input_names[] = {"speech_tokens", "speaker_embeddings", "speaker_features"};
        const char* output_names[] = {"waveform"};

        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(tokens_tensor));
        inputs.push_back(std::move(spk_emb_tensor));
        inputs.push_back(std::move(spk_feat_tensor));

        auto outputs = decoder_session_->Run(
            Ort::RunOptions{nullptr},
            input_names, inputs.data(), inputs.size(),
            output_names, 1);

        float* data = outputs[0].GetTensorMutableData<float>();
        auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t total = 1; for(auto s : out_shape) total *= s;

        return std::vector<float>(data, data + total);
    }

    // =========================================================================
    // Utilities
    // =========================================================================
public:
    void load_tokenizer_vocab(const std::string& vocab_path, const std::string& merges_path) {
        if (vocab_path.empty() || merges_path.empty()) return;
        tokenizer_.load(vocab_path, merges_path);
    }

    std::vector<int64_t> tokenize(const std::string& text) {
        if (!tokenizer_.is_ready()) {
            // Fallback if not loaded
            std::vector<int64_t> ids;
            for (char c : text) {
                ids.push_back(static_cast<int64_t>(c) + 100);
            }
            return ids;
        }
        return tokenizer_.encode(text);
    }

    std::vector<float> concatenate_embeddings(const std::vector<float>& a, const std::vector<float>& b) {
        std::vector<float> result = a;
        result.insert(result.end(), b.begin(), b.end());
        return result;
    }

    void log_model_info(const std::string& name, Ort::Session* session) {
        if (!session) return;
        std::cout << "[Chatterbox] " << name << " loaded." << std::endl;
    }

    // Member variables
    Ort::Env env_;
    Ort::MemoryInfo memory_info_;

    std::unique_ptr<Ort::Session> speech_encoder_session_;
    std::unique_ptr<Ort::Session> embed_tokens_session_;
    std::unique_ptr<Ort::Session> language_model_session_;
    std::unique_ptr<Ort::Session> decoder_session_;

    std::mt19937 rng_;
    std::string current_text_;
    std::vector<float> generated_audio_;
    int num_threads_ = 2;
    bool use_gpu_ = false;
    bool ready_ = false;

    BPETokenizer tokenizer_;
};

} // namespace stts
