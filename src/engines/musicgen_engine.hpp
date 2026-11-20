#pragma once

#include "framework/inference_engine.hpp"
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <random>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace stts {

class MusicGenEngine : public InferenceEngine {
public:
    MusicGenEngine() :
        env_(ORT_LOGGING_LEVEL_WARNING, "MusicGenEngine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)),
        rng_(std::random_device{}())
    {}

    ~MusicGenEngine() {
        cleanup_fallback();
    }

    bool initialize(const std::string& model_dir) override {
        base_path_ = model_dir;
        fallback_data_ = nullptr;
        fallback_size_ = 0;

        size_t model_count = 0;

        try {
            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(2);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            std::string enc_path = base_path_ + "/text_encoder.onnx";
            std::ifstream enc_test(enc_path);
            if (enc_test.good()) {
                enc_test.close();
                text_enc_ = std::make_unique<Ort::Session>(env_, enc_path.c_str(), opts);
                log_model_info("TextEncoder", text_enc_.get());
                parse_encoder_io(text_enc_.get());
                model_count++;
            }

            std::string dec_path = base_path_ + "/musicgen_decoder.onnx";
            std::ifstream dec_test(dec_path);
            if (dec_test.good()) {
                dec_test.close();
                decoder_ = std::make_unique<Ort::Session>(env_, dec_path.c_str(), opts);
                log_model_info("MusicGenDecoder", decoder_.get());
                parse_decoder_io(decoder_.get());
                model_count++;
            }

            std::string encdec_path = base_path_ + "/encodec_decoder.onnx";
            std::ifstream encdec_test(encdec_path);
            if (encdec_test.good()) {
                encdec_test.close();
                encodec_ = std::make_unique<Ort::Session>(env_, encdec_path.c_str(), opts);
                log_model_info("EncodecDecoder", encodec_.get());
                parse_encodec_io(encodec_.get());
                model_count++;
            }

            // Load tokenizer (vocab.json or spiece.model)
            load_tokenizer(base_path_);

            if (model_count > 0) {
                is_loaded_ = true;
                blog(LOG_INFO, "[MusicGen] Initialized with %zu ONNX models", model_count);
                return true;
            }

            blog(LOG_WARNING, "[MusicGen] No ONNX models found in %s", model_dir.c_str());
            return false;
        } catch (const std::exception& e) {
            blog(LOG_ERROR, "[MusicGen] Init error: %s", e.what());
            return false;
        }
    }

    std::string get_name() const override { return "MusicGen Engine"; }
    bool is_ready() const override { return is_loaded_; }

    std::vector<float> process(const std::vector<float>&, uint32_t) override {
        // MusicGen is a generator, not a filter
        return {};
    }

    std::vector<float> generate(const std::string& text, float duration_seconds) {
        if (!is_loaded_) return generate_fallback(duration_seconds);

        try {
            size_t target_samples = (size_t)(duration_seconds * 32000);

            // 1. Tokenize text (T5 SentencePiece or fallback char encoding)
            std::vector<int64_t> input_ids = tokenize_text(text);
            if (input_ids.empty()) {
                return generate_fallback(duration_seconds);
            }

            // Limit sequence length
            if (input_ids.size() > text_enc_max_len_) {
                input_ids.resize(text_enc_max_len_);
            }

            // 2. Run text encoder (T5) to get encoder hidden states
            std::vector<float> encoder_hidden = run_text_encoder(input_ids);
            if (encoder_hidden.empty()) {
                return generate_fallback(duration_seconds);
            }

            // 3. Autoregressive decoding loop (MusicGen delayed pattern)
            std::vector<std::vector<int64_t>> codebooks = run_decoder_loop(
                encoder_hidden, target_samples);

            if (codebooks.empty() || codebooks[0].empty()) {
                return generate_fallback(duration_seconds);
            }

            // 4. Decode audio tokens to waveform via Encodec
            std::vector<float> audio = run_encodec_decode(codebooks);
            if (audio.empty()) {
                return generate_fallback(duration_seconds);
            }

            // Trim/pad to target duration
            if (audio.size() > target_samples) {
                audio.resize(target_samples);
            } else if (audio.size() < target_samples) {
                audio.resize(target_samples, 0.0f);
            }

            return audio;

        } catch (const std::exception& e) {
            blog(LOG_ERROR, "[MusicGen] Generate error: %s", e.what());
            return generate_fallback(duration_seconds);
        }
    }

    // Set a pre-built fallback WAV to use when models aren't available
    void set_fallback_wav(const float* data, size_t num_samples) {
        cleanup_fallback();
        if (data && num_samples > 0) {
            fallback_data_ = new float[num_samples];
            memcpy(fallback_data_, data, num_samples * sizeof(float));
            fallback_size_ = num_samples;
        }
    }

private:
    Ort::Env env_;
    Ort::MemoryInfo memory_info_;
    std::unique_ptr<Ort::Session> text_enc_;
    std::unique_ptr<Ort::Session> decoder_;
    std::unique_ptr<Ort::Session> encodec_;
    std::mt19937 rng_;

    std::string base_path_;
    bool is_loaded_ = false;

    // Fallback WAV data (optional override for sine drone)
    float* fallback_data_ = nullptr;
    size_t fallback_size_ = 0;

    // Model I/O metadata
    std::vector<const char*> enc_input_names_;
    std::vector<const char*> enc_output_names_;
    std::vector<std::string> enc_input_names_str_;
    std::vector<std::string> enc_output_names_str_;
    int64_t text_enc_max_len_ = 256;

    std::vector<const char*> dec_input_names_;
    std::vector<const char*> dec_output_names_;
    std::vector<std::string> dec_input_names_str_;
    std::vector<std::string> dec_output_names_str_;

    std::vector<const char*> encdec_input_names_;
    std::vector<const char*> encdec_output_names_;
    std::vector<std::string> encdec_input_names_str_;
    std::vector<std::string> encdec_output_names_str_;

    // Tokenizer state
    std::unordered_map<std::string, int64_t> token_to_id_;
    std::unordered_map<int64_t, std::string> id_to_token_;
    bool tokenizer_loaded_ = false;

    // Constants for MusicGen
    static constexpr int NUM_CODEBOOKS = 4;
    static constexpr int MAX_GEN_TOKENS = 1500;
    static constexpr int START_TOKEN = 2048;
    static constexpr float TEMPERATURE = 0.9f;

    // =========================================================================
    // Tokenizer (Minimal T5 SentencePiece support)
    // =========================================================================
    void load_tokenizer(const std::string& base_path) {
        std::string vocab_path = base_path + "/vocab.json";
        std::ifstream f(vocab_path);
        if (f.good()) {
            f.close();
            load_vocab_json(vocab_path);
            return;
        }

        // Try spiece.model (SentencePiece protobuf)
        std::string sp_path = base_path + "/spiece.model";
        std::ifstream sf(sp_path);
        if (sf.good()) {
            sf.close();
            // Attempt minimal SentencePiece vocab extraction
            load_spiece_model(sp_path);
            return;
        }

        blog(LOG_WARNING, "[MusicGen] No tokenizer found, using char fallback");
    }

    void load_vocab_json(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) return;

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        // Minimal JSON parsing for token->id map
        // Expects format: {"token1": id1, "token2": id2, ...}
        size_t pos = 1; // skip initial {
        while (pos < content.size()) {
            pos = content.find('"', pos);
            if (pos == std::string::npos) break;
            size_t key_start = pos + 1;
            size_t key_end = content.find('"', key_start);
            if (key_end == std::string::npos) break;
            std::string token = content.substr(key_start, key_end - key_start);

            pos = content.find(':', key_end);
            if (pos == std::string::npos) break;

            // Escape sequences in token
            size_t val_start = pos + 1;
            while (val_start < content.size() &&
                   (content[val_start] == ' ' || content[val_start] == '\t' ||
                    content[val_start] == '\n' || content[val_start] == '\r'))
                val_start++;

            char* end = nullptr;
            int64_t id = std::strtoll(content.c_str() + val_start, &end, 10);
            if (end == content.c_str() + val_start) break;

            token_to_id_[token] = id;
            id_to_token_[id] = token;

            pos = end - content.c_str();
        }

        if (!token_to_id_.empty()) {
            tokenizer_loaded_ = true;
            blog(LOG_INFO, "[MusicGen] Loaded %zu vocab entries", token_to_id_.size());
        }
    }

    void load_spiece_model(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return;

        // SentencePiece .model is a serialized protobuf.
        // We extract the "pieces" array by scanning for known patterns.
        // This is fragile but works for common SP models.
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        // Look for the pieces array in the protobuf.
        // Format: ... piece { ... piece: "token" ... score: ... } ...
        std::string piece_marker = "piece:";
        std::string score_marker = "score:";
        size_t search_pos = 0;
        int64_t id = 0;

        while (id < 50000) {
            size_t p = content.find(piece_marker, search_pos);
            if (p == std::string::npos) break;

            // Find the string value
            size_t quote_start = content.find('"', p + piece_marker.size());
            if (quote_start == std::string::npos) break;
            size_t quote_end = content.find('"', quote_start + 1);
            if (quote_end == std::string::npos) break;

            std::string token = content.substr(quote_start + 1,
                                                quote_end - quote_start - 1);

            token_to_id_[token] = id;
            id_to_token_[id] = token;
            id++;

            search_pos = quote_end + 1;
        }

        if (id > 0) {
            tokenizer_loaded_ = true;
            blog(LOG_INFO, "[MusicGen] Loaded %lld SentencePiece entries", (long long)id);
        }
    }

    std::vector<int64_t> tokenize_text(const std::string& text) {
        if (tokenizer_loaded_ && !token_to_id_.empty()) {
            // Simple word-level tokenization with vocab lookup
            std::vector<int64_t> ids;
            std::string word;
            for (char c : text) {
                if (c == ' ') {
                    if (!word.empty()) {
                        lookup_or_fallback(word, ids);
                        word.clear();
                    }
                    // Add space token
                    auto it = token_to_id_.find("\u2581"); // SentencePiece space marker
                    if (it != token_to_id_.end()) ids.push_back(it->second);
                } else {
                    word += c;
                }
            }
            if (!word.empty()) lookup_or_fallback(word, ids);
            return ids;
        }

        // Char-level fallback
        std::vector<int64_t> ids;
        ids.push_back(1); // Start token
        for (char c : text) {
            ids.push_back(static_cast<int64_t>(c) + 10);
        }
        ids.push_back(1); // End token
        return ids;
    }

    void lookup_or_fallback(const std::string& word, std::vector<int64_t>& ids) {
        // Try exact match
        auto it = token_to_id_.find(word);
        if (it != token_to_id_.end()) {
            ids.push_back(it->second);
            return;
        }

        // Try lowercase
        std::string lower = word;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        it = token_to_id_.find(lower);
        if (it != token_to_id_.end()) {
            ids.push_back(it->second);
            return;
        }

        // Char-level fallback
        for (char c : word) {
            std::string s(1, c);
            it = token_to_id_.find(s);
            if (it != token_to_id_.end()) {
                ids.push_back(it->second);
            } else {
                it = token_to_id_.find("<unk>");
                if (it != token_to_id_.end()) ids.push_back(it->second);
            }
        }
    }

    // =========================================================================
    // ONNX Model I/O Parsing
    // =========================================================================
    void parse_encoder_io(Ort::Session* session) {
        Ort::AllocatorWithDefaultOptions alloc;
        enc_input_names_str_.clear();
        enc_output_names_str_.clear();
        enc_input_names_.clear();
        enc_output_names_.clear();

        for (size_t i = 0; i < session->GetInputCount(); i++) {
            auto name = session->GetInputNameAllocated(i, alloc);
            enc_input_names_str_.push_back(name.get());
            enc_input_names_.push_back(enc_input_names_str_.back().c_str());

            // Check for max length constraint
            auto type_info = session->GetInputTypeInfo(i);
            auto shape = type_info.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 2 && shape[1] > 0) {
                text_enc_max_len_ = shape[1];
            }
        }
        for (size_t i = 0; i < session->GetOutputCount(); i++) {
            auto name = session->GetOutputNameAllocated(i, alloc);
            enc_output_names_str_.push_back(name.get());
            enc_output_names_.push_back(enc_output_names_str_.back().c_str());
        }
    }

    void parse_decoder_io(Ort::Session* session) {
        Ort::AllocatorWithDefaultOptions alloc;
        dec_input_names_str_.clear();
        dec_output_names_str_.clear();
        dec_input_names_.clear();
        dec_output_names_.clear();

        for (size_t i = 0; i < session->GetInputCount(); i++) {
            auto name = session->GetInputNameAllocated(i, alloc);
            dec_input_names_str_.push_back(name.get());
            dec_input_names_.push_back(dec_input_names_str_.back().c_str());
        }
        for (size_t i = 0; i < session->GetOutputCount(); i++) {
            auto name = session->GetOutputNameAllocated(i, alloc);
            dec_output_names_str_.push_back(name.get());
            dec_output_names_.push_back(dec_output_names_str_.back().c_str());
        }
    }

    void parse_encodec_io(Ort::Session* session) {
        Ort::AllocatorWithDefaultOptions alloc;
        encdec_input_names_str_.clear();
        encdec_output_names_str_.clear();
        encdec_input_names_.clear();
        encdec_output_names_.clear();

        for (size_t i = 0; i < session->GetInputCount(); i++) {
            auto name = session->GetInputNameAllocated(i, alloc);
            encdec_input_names_str_.push_back(name.get());
            encdec_input_names_.push_back(encdec_input_names_str_.back().c_str());
        }
        for (size_t i = 0; i < session->GetOutputCount(); i++) {
            auto name = session->GetOutputNameAllocated(i, alloc);
            encdec_output_names_str_.push_back(name.get());
            encdec_output_names_.push_back(encdec_output_names_str_.back().c_str());
        }
    }

    // =========================================================================
    // Text Encoder (T5)
    // =========================================================================
    std::vector<float> run_text_encoder(const std::vector<int64_t>& input_ids) {
        if (!text_enc_) return {};

        std::vector<int64_t> shape = {1, (int64_t)input_ids.size()};

        Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(
            memory_info_, const_cast<int64_t*>(input_ids.data()), input_ids.size(),
            shape.data(), shape.size());

        // Use encoder I/O names
        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(input_tensor));

        // Add attention mask if expected
        if (enc_input_names_.size() > 1) {
            std::vector<int64_t> mask(input_ids.size(), 1);
            std::vector<int64_t> mask_shape = {1, (int64_t)input_ids.size()};
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memory_info_, mask.data(), mask.size(),
                mask_shape.data(), mask_shape.size()));
        }

        auto outputs = text_enc_->Run(
            Ort::RunOptions{nullptr},
            enc_input_names_.data(), inputs.data(), inputs.size(),
            enc_output_names_.data(), enc_output_names_.size());

        if (outputs.empty()) return {};

        float* data = outputs[0].GetTensorMutableData<float>();
        size_t total = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

        return std::vector<float>(data, data + total);
    }

    // =========================================================================
    // MusicGen Decoder (Autoregressive with Delayed Pattern)
    // =========================================================================
    std::vector<std::vector<int64_t>> run_decoder_loop(
        const std::vector<float>& encoder_hidden,
        size_t target_samples)
    {
        if (!decoder_) return {};

        // Calculate number of audio tokens needed
        // Encodec operates at ~50 tokens/sec at 32kHz
        // MusicGen frame rate: 50 Hz (20ms per frame)
        size_t num_steps = target_samples / 640 + 1; // 32000/50 = 640 samples per step
        if (num_steps > (size_t)MAX_GEN_TOKENS) num_steps = MAX_GEN_TOKENS;

        // Initialize codebooks with start tokens (delayed pattern)
        // Codebook 0: [START_TOKEN, ...]
        // Codebook 1: [PAD, START_TOKEN, ...]  (offset by 1)
        // Codebook 2: [PAD, PAD, START_TOKEN, ...] (offset by 2)
        // Codebook 3: [PAD, PAD, PAD, START_TOKEN, ...] (offset by 3)
        std::vector<std::vector<int64_t>> codebooks(NUM_CODEBOOKS);
        for (int cb = 0; cb < NUM_CODEBOOKS; cb++) {
            for (int j = 0; j < cb; j++) {
                codebooks[cb].push_back(0); // Pad
            }
            codebooks[cb].push_back(START_TOKEN);
        }

        // Get encoder hidden state shape
        auto enc_shape = decoder_->GetInputTypeInfo(1).GetTensorTypeAndShapeInfo().GetShape();
        int64_t enc_seq_len = (enc_shape.size() >= 2 && enc_shape[1] > 0)
            ? enc_shape[1] : (encoder_hidden.size() / 1024);
        int64_t hidden_dim = (enc_shape.size() >= 3 && enc_shape[2] > 0)
            ? enc_shape[2] : 1024;

        // Truncate or pad encoder hidden to match expected shape
        size_t expected_enc_size = (size_t)(enc_seq_len * hidden_dim);
        std::vector<float> enc_hidden_adapted = encoder_hidden;
        if (enc_hidden_adapted.size() > expected_enc_size) {
            enc_hidden_adapted.resize(expected_enc_size);
        } else if (enc_hidden_adapted.size() < expected_enc_size) {
            enc_hidden_adapted.resize(expected_enc_size, 0.0f);
        }

        // Autoregressive loop
        for (int step = 0; step < (int)num_steps - 1; step++) {
            try {
                // Build input sequence: all codebook tokens up to current step
                // In delayed pattern, each codebook has a different effective length
                int current_len = codebooks[0].size();

                // Prepare input tensors
                std::vector<Ort::Value> inputs;

                // 1. Input IDs (all codebooks concatenated)
                // Some models expect [1, seq_len] for each codebook
                // MusicGen ONNX usually expects input_ids with shape [1, seq_len]
                // We'll use the first codebook as primary input
                std::vector<int64_t> input_ids = codebooks[0];
                std::vector<int64_t> input_shape = {1, (int64_t)input_ids.size()};

                inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                    memory_info_, input_ids.data(), input_ids.size(),
                    input_shape.data(), input_shape.size()));

                // 2. Encoder hidden states
                std::vector<int64_t> enc_shape_arr = {1, enc_seq_len, hidden_dim};
                inputs.push_back(Ort::Value::CreateTensor<float>(
                    memory_info_, enc_hidden_adapted.data(), enc_hidden_adapted.size(),
                    enc_shape_arr.data(), enc_shape_arr.size()));

                // Run decoder
                auto outputs = decoder_->Run(
                    Ort::RunOptions{nullptr},
                    dec_input_names_.data(), inputs.data(), inputs.size(),
                    dec_output_names_.data(), dec_output_names_.size());

                // Parse logits and sample next tokens for each codebook
                // Output shape: [1, seq_len, vocab_size * 4] or [4, seq_len, vocab_size]
                float* logits_data = outputs[0].GetTensorMutableData<float>();
                auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
                int64_t vocab_size = out_shape[out_shape.size() - 1] / NUM_CODEBOOKS;

                // Sample from last position for each codebook
                for (int cb = 0; cb < NUM_CODEBOOKS; cb++) {
                    int64_t offset = cb * vocab_size;
                    int64_t flat_idx = (current_len - 1) * out_shape[out_shape.size() - 1] + offset;
                    int64_t next_token = sample(logits_data + flat_idx, vocab_size, TEMPERATURE);
                    codebooks[cb].push_back((int64_t)next_token);
                }

            } catch (const std::exception& e) {
                blog(LOG_ERROR, "[MusicGen] Decoder step %d error: %s", step, e.what());
                break;
            }
        }

        return codebooks;
    }

    // =========================================================================
    // Encodec Decoder (Audio Tokens to Waveform)
    // =========================================================================
    std::vector<float> run_encodec_decode(const std::vector<std::vector<int64_t>>& codebooks) {
        if (!encodec_) return {};

        // Codebooks shape: [1, 4, seq_len]
        int64_t seq_len = (int64_t)codebooks[0].size();
        std::vector<int64_t> flat_codes;
        for (int i = 0; i < seq_len; i++) {
            for (int cb = 0; cb < NUM_CODEBOOKS; cb++) {
                if (i < (int)codebooks[cb].size()) {
                    flat_codes.push_back(codebooks[cb][i]);
                } else {
                    flat_codes.push_back(0);
                }
            }
        }

        std::vector<int64_t> shape = {1, NUM_CODEBOOKS, seq_len};

        Ort::Value codes_tensor = Ort::Value::CreateTensor<int64_t>(
            memory_info_, flat_codes.data(), flat_codes.size(),
            shape.data(), shape.size());

        auto outputs = encodec_->Run(
            Ort::RunOptions{nullptr},
            encdec_input_names_.data(), &codes_tensor, 1,
            encdec_output_names_.data(), encdec_output_names_.size());

        if (outputs.empty()) return {};

        float* audio_data = outputs[0].GetTensorMutableData<float>();
        size_t total_samples = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

        return std::vector<float>(audio_data, audio_data + total_samples);
    }

    // =========================================================================
    // Sampling Utilities
    // =========================================================================
    int64_t sample(float* logits, int64_t vocab_size, float temperature) {
        // Find top-k to avoid sampling from the long tail
        constexpr int TOP_K = 50;

        // Gather top-k indices and values
        std::vector<std::pair<float, int>> scored;
        scored.reserve(vocab_size);
        for (int64_t i = 0; i < vocab_size; i++) {
            scored.push_back({logits[i] / temperature, (int)i});
        }

        // Partial sort for top-k
        std::partial_sort(scored.begin(), scored.begin() + TOP_K, scored.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

        // Softmax over top-k
        float max_val = scored[0].first;
        float sum_exp = 0.0f;
        for (int i = 0; i < TOP_K; i++) {
            scored[i].first = std::exp(scored[i].first - max_val);
            sum_exp += scored[i].first;
        }

        // Sample
        std::uniform_real_distribution<float> dist(0.0f, sum_exp);
        float sample = dist(rng_);
        float cum = 0.0f;
        for (int i = 0; i < TOP_K; i++) {
            cum += scored[i].first;
            if (sample <= cum) return scored[i].second;
        }

        return scored[TOP_K - 1].second;
    }

    // =========================================================================
    // Fallback Generator
    // =========================================================================
    std::vector<float> generate_fallback(float duration) {
        // Use pre-loaded WAV if available
        if (fallback_data_ && fallback_size_ > 0) {
            size_t samples = (size_t)(duration * 32000);
            std::vector<float> audio(samples);
            for (size_t i = 0; i < samples; i++) {
                audio[i] = fallback_data_[i % fallback_size_];
            }
            return audio;
        }

        // Default: sci-fi drone
        size_t samples = (size_t)(duration * 32000);
        std::vector<float> audio(samples);
        float phase = 0;
        for (size_t i = 0; i < samples; ++i) {
            float t = (float)i / 32000.0f;
            float carrier = std::sin(phase);
            float mod = std::sin(t * 5.0f * 6.28f);
            audio[i] = carrier * (0.5f + 0.5f * mod) * 0.1f;
            phase += (440.0f + 50.0f * std::sin(t * 0.5f)) * 6.28f / 32000.0f;
            if (phase > 6.28f) phase -= 6.28f;
        }
        return audio;
    }

    void cleanup_fallback() {
        if (fallback_data_) {
            delete[] fallback_data_;
            fallback_data_ = nullptr;
            fallback_size_ = 0;
        }
    }

    void log_model_info(const std::string& name, Ort::Session* session) {
        if (!session) return;
        Ort::AllocatorWithDefaultOptions allocator;
        blog(LOG_INFO, "[MusicGen] %s model loaded", name.c_str());
    }
};

} // namespace stts
