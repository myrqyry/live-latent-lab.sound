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
#include <complex>

// pocketfft header-only FFT library
#include <pocketfft_hdronly.h>

namespace stts {

/**
 * @class WhisperEngine
 * @brief Speech-to-Text engine using OpenAI Whisper ONNX models.
 * 
 * Supports various Whisper ONNX formats:
 * - onnx-community/whisper-base, whisper-small, whisper-medium, whisper-large
 * - Optimized variants (INT4/INT8 quantized)
 * 
 * Pipeline:
 * 1. Resample audio to 16kHz (Whisper's native rate)
 * 2. Compute log-mel spectrogram (80 mel bins) using pocketfft
 * 3. Run encoder to get audio embeddings
 * 4. Run decoder autoregressively to generate tokens
 * 5. Decode tokens to text using BPE tokenizer
 * 
 * Model Sources:
 * - onnx-community: https://huggingface.co/onnx-community/whisper-base
 * - Optimized: https://huggingface.co/Intel/whisper-small-onnx-int4-inc
 * 
 * Note: For real-time STT, consider whisper.cpp which has lower latency.
 */
class WhisperEngine : public InferenceEngine {
public:
    WhisperEngine() : 
        env_(ORT_LOGGING_LEVEL_WARNING, "Whisper_Engine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        init_mel_filters();
        init_hann_window();
    }

    virtual bool initialize(const std::string& config) override {
        try {
            // Parse config: encoder_path|decoder_path|vocab_path|merges_path|threads|language|backend
            std::stringstream ss(config);
            std::string encoder_path, decoder_path, vocab_path, merges_path, threads_str, language, backend_str;
            std::getline(ss, encoder_path, '|');
            std::getline(ss, decoder_path, '|');
            std::getline(ss, vocab_path, '|');
            std::getline(ss, merges_path, '|');
            std::getline(ss, threads_str, '|');
            std::getline(ss, language, '|');
            std::getline(ss, backend_str, '|');

            num_threads_ = 2;
            if (!threads_str.empty()) num_threads_ = std::stoi(threads_str);
            language_ = language.empty() ? "en" : language;

            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(num_threads_);
            
            // Configure GPU if available
            gpu::GPUConfig gpu_config;
            gpu_config.backend = gpu::parse_backend(backend_str.empty() ? "auto" : backend_str);
            use_gpu_ = gpu::configure_gpu_session(session_options, gpu_config);

            // Load models
            if (!encoder_path.empty()) {
                encoder_session_ = std::make_unique<Ort::Session>(env_, encoder_path.c_str(), session_options);
                log_model_info("Encoder", encoder_session_.get());
            }
            if (!decoder_path.empty()) {
                decoder_session_ = std::make_unique<Ort::Session>(env_, decoder_path.c_str(), session_options);
                log_model_info("Decoder", decoder_session_.get());
            }

            // Load tokenizer
            if (!vocab_path.empty() && !merges_path.empty()) {
                if (tokenizer_.load(vocab_path, merges_path)) {
                    std::cout << "[Whisper] Tokenizer loaded successfully" << std::endl;
                } else {
                    std::cerr << "[Whisper] Failed to load tokenizer" << std::endl;
                }
            }

            ready_ = (encoder_session_ != nullptr);
            std::cout << "[Whisper] Initialized with " << num_threads_ << " threads"
                      << (use_gpu_ ? " (GPU)" : " (CPU)")
                      << ", language: " << language_ << std::endl;
            return ready_;
        } catch (const std::exception& e) {
            std::cerr << "[Whisper] Init error: " << e.what() << std::endl;
            ready_ = false;
            return false;
        }
    }

    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (ready_ && encoder_session_) {
            last_transcription_ = transcribe(input, sample_rate);
        }
        return input;
    }

    virtual std::string transcribe(const std::vector<float>& audio, uint32_t sample_rate) override {
        if (!ready_ || !encoder_session_) return "";

        try {
            // Step 1: Resample to 16kHz
            std::vector<float> audio_16k = Resampler::resample(audio, sample_rate, 16000);
            
            // Step 2: Pad/truncate to 30 seconds
            const size_t target_samples = 16000 * 30;
            audio_16k.resize(target_samples, 0.0f);
            
            // Step 3: Compute mel spectrogram using FFT
            std::vector<float> mel_spec = compute_mel_spectrogram_fft(audio_16k);
            
            // Step 4: Run encoder
            std::vector<float> encoder_output = run_encoder(mel_spec);
            
            // Step 5: Run decoder autoregressively
            std::vector<int64_t> tokens;
            if (decoder_session_) {
                tokens = run_decoder_autoregressive(encoder_output);
            }
            
            // Step 6: Decode tokens to text
            return decode_tokens(tokens);
        } catch (const std::exception& e) {
            std::cerr << "[Whisper] Transcribe error: " << e.what() << std::endl;
            return "";
        }
    }

    std::string get_last_transcription() const { return last_transcription_; }
    virtual std::string get_name() const override { return "Whisper (ONNX STT)"; }
    virtual bool is_ready() const override { return ready_; }

private:
    // =========================================================================
    // FFT-based Mel Spectrogram Computation
    // =========================================================================
    void init_hann_window() {
        hann_window_.resize(N_FFT);
        for (int i = 0; i < N_FFT; i++) {
            hann_window_[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (N_FFT - 1)));
        }
    }

    void init_mel_filters() {
        mel_filters_.resize(N_MEL * (N_FFT / 2 + 1), 0.0f);
        
        float mel_low = hz_to_mel(0);
        float mel_high = hz_to_mel(8000);
        
        std::vector<float> mel_points(N_MEL + 2);
        for (int i = 0; i < N_MEL + 2; i++) {
            mel_points[i] = mel_to_hz(mel_low + (mel_high - mel_low) * i / (N_MEL + 1));
        }
        
        std::vector<int> bin_points(N_MEL + 2);
        for (int i = 0; i < N_MEL + 2; i++) {
            bin_points[i] = static_cast<int>(std::floor((N_FFT + 1) * mel_points[i] / 16000));
        }
        
        for (int m = 0; m < N_MEL; m++) {
            for (int k = bin_points[m]; k < bin_points[m + 1]; k++) {
                if (k < N_FFT / 2 + 1) {
                    mel_filters_[m * (N_FFT / 2 + 1) + k] = 
                        static_cast<float>(k - bin_points[m]) / (bin_points[m + 1] - bin_points[m]);
                }
            }
            for (int k = bin_points[m + 1]; k < bin_points[m + 2]; k++) {
                if (k < N_FFT / 2 + 1) {
                    mel_filters_[m * (N_FFT / 2 + 1) + k] = 
                        static_cast<float>(bin_points[m + 2] - k) / (bin_points[m + 2] - bin_points[m + 1]);
                }
            }
        }
    }

    float hz_to_mel(float hz) { return 2595.0f * std::log10(1.0f + hz / 700.0f); }
    float mel_to_hz(float mel) { return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f); }

    std::vector<float> compute_mel_spectrogram_fft(const std::vector<float>& audio) {
        const int num_frames = (static_cast<int>(audio.size()) - N_FFT) / HOP_LENGTH + 1;
        std::vector<float> mel_spec(N_MEL * num_frames, 0.0f);
        
        pocketfft::shape_t shape = {static_cast<size_t>(N_FFT)};
        pocketfft::stride_t stride_in = {sizeof(std::complex<float>)};
        pocketfft::stride_t stride_out = {sizeof(std::complex<float>)};
        pocketfft::shape_t axes = {0};
        
        std::vector<std::complex<float>> fft_input(N_FFT);
        std::vector<std::complex<float>> fft_output(N_FFT);
        std::vector<float> power_spec(N_FFT / 2 + 1);
        
        for (int frame = 0; frame < num_frames; frame++) {
            int start = frame * HOP_LENGTH;
            
            for (int n = 0; n < N_FFT; n++) {
                float sample = (start + n < static_cast<int>(audio.size())) ? audio[start + n] : 0.0f;
                fft_input[n] = std::complex<float>(sample * hann_window_[n], 0.0f);
            }
            
            pocketfft::c2c(shape, stride_in, stride_out, axes, 
                          pocketfft::FORWARD, fft_input.data(), fft_output.data(), 1.0f);
            
            for (int k = 0; k < N_FFT / 2 + 1; k++) {
                float real = fft_output[k].real();
                float imag = fft_output[k].imag();
                power_spec[k] = real * real + imag * imag;
            }
            
            for (int m = 0; m < N_MEL; m++) {
                float mel_energy = 0.0f;
                for (int k = 0; k < N_FFT / 2 + 1; k++) {
                    mel_energy += mel_filters_[m * (N_FFT / 2 + 1) + k] * power_spec[k];
                }
                mel_spec[m * num_frames + frame] = std::log(std::max(mel_energy, 1e-10f));
            }
        }
        
        float max_val = *std::max_element(mel_spec.begin(), mel_spec.end());
        for (auto& v : mel_spec) {
            v = std::max((v - max_val) / 8.0f + 1.0f, 0.0f);
        }
        
        return mel_spec;
    }

    // =========================================================================
    // Encoder
    // =========================================================================
    std::vector<float> run_encoder(const std::vector<float>& mel_spec) {
        const int64_t n_frames = mel_spec.size() / N_MEL;
        std::vector<int64_t> shape = {1, N_MEL, n_frames};
        
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, const_cast<float*>(mel_spec.data()), mel_spec.size(),
            shape.data(), shape.size());

        std::vector<const char*> input_names = {"input_features"};
        std::vector<const char*> output_names = {"last_hidden_state"};
        
        auto outputs = encoder_session_->Run(
            Ort::RunOptions{nullptr},
            input_names.data(), &input_tensor, 1,
            output_names.data(), output_names.size());

        float* data = outputs[0].GetTensorMutableData<float>();
        auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t total = 1;
        for (auto s : out_shape) total *= s;

        return std::vector<float>(data, data + total);
    }

    // =========================================================================
    // Decoder (Autoregressive)
    // =========================================================================
    std::vector<int64_t> run_decoder_autoregressive(const std::vector<float>& encoder_output) {
        // Whisper special tokens
        constexpr int64_t SOT = 50258;        // start of transcript
        constexpr int64_t EOT = 50257;        // end of transcript
        constexpr int64_t TRANSCRIBE = 50359; // transcribe task
        constexpr int64_t NO_TIMESTAMPS = 50363;
        constexpr int MAX_TOKENS = 448;
        
        std::vector<int64_t> tokens;
        
        // Initialize with start tokens
        tokens.push_back(SOT);
        tokens.push_back(50259 + get_language_id(language_)); // Language token
        tokens.push_back(TRANSCRIBE);
        tokens.push_back(NO_TIMESTAMPS);
        
        if (!decoder_session_) {
            return tokens;
        }
        
        // Get encoder output shape for cross-attention
        // Typically [1, seq_len, hidden_dim] e.g., [1, 1500, 512] for whisper-base
        const size_t enc_size = encoder_output.size();
        
        // Autoregressive decoding loop
        for (int step = 0; step < MAX_TOKENS; step++) {
            try {
                // Create input tensor for decoder_input_ids
                std::vector<int64_t> input_ids_shape = {1, static_cast<int64_t>(tokens.size())};
                Ort::Value input_ids_tensor = Ort::Value::CreateTensor<int64_t>(
                    memory_info_, tokens.data(), tokens.size(),
                    input_ids_shape.data(), input_ids_shape.size());
                
                // Create encoder hidden states tensor
                // Assuming encoder output is [1, seq_len, hidden_dim]
                int64_t hidden_dim = 512; // whisper-base default
                int64_t seq_len = enc_size / hidden_dim;
                std::vector<int64_t> enc_shape = {1, seq_len, hidden_dim};
                Ort::Value encoder_tensor = Ort::Value::CreateTensor<float>(
                    memory_info_, const_cast<float*>(encoder_output.data()), enc_size,
                    enc_shape.data(), enc_shape.size());
                
                // Run decoder
                std::vector<const char*> input_names = {"input_ids", "encoder_hidden_states"};
                std::vector<const char*> output_names = {"logits"};
                std::vector<Ort::Value> inputs;
                inputs.push_back(std::move(input_ids_tensor));
                inputs.push_back(std::move(encoder_tensor));
                
                auto outputs = decoder_session_->Run(
                    Ort::RunOptions{nullptr},
                    input_names.data(), inputs.data(), inputs.size(),
                    output_names.data(), output_names.size());
                
                // Get logits and find argmax for greedy decoding
                float* logits = outputs[0].GetTensorMutableData<float>();
                auto logits_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
                
                // logits shape is [batch, seq_len, vocab_size]
                int64_t vocab_size = logits_shape[2];
                int64_t last_pos = logits_shape[1] - 1;
                
                // Find argmax at the last position
                float max_val = -1e30f;
                int64_t next_token = 0;
                for (int64_t v = 0; v < vocab_size; v++) {
                    float val = logits[last_pos * vocab_size + v];
                    if (val > max_val) {
                        max_val = val;
                        next_token = v;
                    }
                }
                
                // Check for end of transcript
                if (next_token == EOT) {
                    break;
                }
                
                tokens.push_back(next_token);
                
            } catch (const std::exception& e) {
                std::cerr << "[Whisper] Decoder step " << step << " error: " << e.what() << std::endl;
                break;
            }
        }
        
        return tokens;
    }

    int get_language_id(const std::string& lang) {
        static std::unordered_map<std::string, int> lang_ids = {
            {"en", 0}, {"zh", 1}, {"de", 2}, {"es", 3}, {"ru", 4}, 
            {"ko", 5}, {"fr", 6}, {"ja", 7}, {"pt", 8}, {"tr", 9},
            {"pl", 10}, {"nl", 11}, {"it", 12}, {"sv", 13}, {"cs", 14}
        };
        auto it = lang_ids.find(lang);
        return (it != lang_ids.end()) ? it->second : 0;
    }

    // =========================================================================
    // Token Decoding
    // =========================================================================
    std::string decode_tokens(const std::vector<int64_t>& tokens) {
        if (!tokenizer_.is_ready()) {
            // Fallback: return indicator if tokenizer not loaded
            return "[Tokenizer not loaded]";
        }
        
        // Filter out special tokens before decoding
        std::vector<int64_t> text_tokens;
        for (int64_t tok : tokens) {
            // Skip special tokens (Whisper uses tokens >= 50256 for special purposes)
            if (!tokenizer_.is_special_token(tok)) {
                text_tokens.push_back(tok);
            }
        }
        
        return tokenizer_.decode(text_tokens);
    }

    // =========================================================================
    // Utilities
    // =========================================================================
    void log_model_info(const std::string& name, Ort::Session* session) {
        if (!session) return;
        
        Ort::AllocatorWithDefaultOptions allocator;
        
        std::cout << "[Whisper] " << name << " model loaded. Inputs: ";
        for (size_t i = 0; i < session->GetInputCount(); ++i) {
            auto input_name = session->GetInputNameAllocated(i, allocator);
            std::cout << input_name.get();
            if (i < session->GetInputCount() - 1) std::cout << ", ";
        }
        std::cout << " | Outputs: ";
        for (size_t i = 0; i < session->GetOutputCount(); ++i) {
            auto output_name = session->GetOutputNameAllocated(i, allocator);
            std::cout << output_name.get();
            if (i < session->GetOutputCount() - 1) std::cout << ", ";
        }
        std::cout << std::endl;
    }

    // Constants
    static constexpr int N_FFT = 400;
    static constexpr int HOP_LENGTH = 160;
    static constexpr int N_MEL = 80;

    // Member variables
    Ort::Env env_;
    Ort::MemoryInfo memory_info_;
    std::unique_ptr<Ort::Session> encoder_session_;
    std::unique_ptr<Ort::Session> decoder_session_;
    BPETokenizer tokenizer_;
    std::vector<float> mel_filters_;
    std::vector<float> hann_window_;
    std::string language_ = "en";
    std::string last_transcription_;
    int num_threads_ = 2;
    bool use_gpu_ = false;
    bool ready_ = false;
};

} // namespace stts
