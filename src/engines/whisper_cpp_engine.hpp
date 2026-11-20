#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include <whisper.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>

namespace stts {

/**
 * @class WhisperCppEngine
 * @brief Speech-to-Text engine using whisper.cpp (GGML models).
 * 
 * Provides high-performance, low-latency transcription.
 */
class WhisperCppEngine : public InferenceEngine {
public:
    WhisperCppEngine() : ctx_(nullptr) {}

    virtual ~WhisperCppEngine() {
        if (ctx_) {
            whisper_free(ctx_);
        }
    }

    virtual bool initialize(const std::string& config) override {
        // Parse config: model_path|threads|language|translate
        std::stringstream ss(config);
        std::string model_path, threads_str, language, translate_str;
        std::getline(ss, model_path, '|');
        std::getline(ss, threads_str, '|');
        std::getline(ss, language, '|');
        std::getline(ss, translate_str, '|');

        if (model_path.empty()) return false;

        std::lock_guard<std::mutex> lock(engine_mutex_);

        if (ctx_) {
            whisper_free(ctx_);
            ctx_ = nullptr;
        }

        struct whisper_context_params cparams = whisper_context_default_params();
        ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);

        if (!ctx_) {
            std::cerr << "[WhisperCpp] Failed to initialize from " << model_path << std::endl;
            return false;
        }

        num_threads_ = threads_str.empty() ? 4 : std::stoi(threads_str);
        language_ = language.empty() ? "en" : language;
        translate_ = (translate_str == "true" || translate_str == "1");

        ready_ = true;
        std::cout << "[WhisperCpp] Initialized with " << num_threads_ << " threads, language: " << language_ << std::endl;
        return true;
    }

    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        // For audio filter chain, pass-through
        if (ready_) {
            last_transcription_ = transcribe(input, sample_rate);
        }
        return input;
    }

    virtual std::string transcribe(const std::vector<float>& audio, uint32_t sample_rate) override {
        if (!ready_ || !ctx_) return "";

        std::lock_guard<std::mutex> lock(engine_mutex_);

        // Resample if necessary (whisper.cpp requires 16000Hz)
        std::vector<float> audio_16k;
        if (sample_rate != 16000) {
            audio_16k = Resampler::resample(audio, sample_rate, 16000);
        } else {
            audio_16k = audio;
        }

        whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.print_progress = false;
        params.print_special = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.translate = translate_;
        params.language = language_.c_str();
        params.n_threads = num_threads_;

        if (whisper_full(ctx_, params, audio_16k.data(), audio_16k.size()) != 0) {
            return "";
        }

        std::string result;
        const int n_segments = whisper_full_n_segments(ctx_);
        for (int i = 0; i < n_segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx_, i);
            result += text;
        }

        return result;
    }

    std::string get_last_transcription() const { return last_transcription_; }

    virtual std::string get_name() const override { return "Whisper (GGML)"; }
    virtual bool is_ready() const override { return ready_; }

private:
    struct whisper_context* ctx_ = nullptr;
    std::mutex engine_mutex_;
    std::string language_ = "en";
    std::string last_transcription_;
    int num_threads_ = 4;
    bool translate_ = false;
    bool ready_ = false;
};

} // namespace stts
