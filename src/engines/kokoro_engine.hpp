#pragma once

#include "framework/inference_engine.hpp"
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <onnxruntime_cxx_api.h>

namespace stts {

class KokoroEngine : public InferenceEngine {
public:
    KokoroEngine() {}

    // Initialize the engine with the .onnx model path
    bool initialize(const std::string& model_path) override {
        std::lock_guard<std::mutex> lock(model_mutex_);
        try {
            // Default "Voice Style" vector (Zeroed for neutral, or load from file)
            current_style_.resize(256, 0.0f);

            // Configure ONNX Session
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "KokoroEngine");
            session_options_.SetIntraOpNumThreads(1); // Keep thread count low for OBS
            session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            // Note: Append CUDA provider here if desired
            
            session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options_);
            is_loaded_ = true;
            return true;
        } catch (const Ort::Exception& e) {
            // error_message_ = e.what();
            is_loaded_ = false;
            return false;
        }
    }

    // Required by InferenceEngine base
    std::string get_name() const override { return "Kokoro-82M TTS"; }
    bool is_ready() const override { return is_loaded_; }
    
    // Stub for base process() which deals with input audio
    std::vector<float> process(const std::vector<float>& /*input*/, uint32_t /*sample_rate*/) override {
        return {};
    }

    /**
     * @brief Generate audio from tokenized phonemes.
     * @param tokens The tokenized phoneme IDs.
     * @param speed Playback speed (1.0 = normal).
     * @return Raw float audio samples at 24kHz.
     */
    std::vector<float> generate(const std::vector<int64_t>& tokens, float speed = 1.0f) {
        if (!is_loaded_ || tokens.empty()) return {};

        std::lock_guard<std::mutex> lock(model_mutex_);

        // Shapes expected by Kokoro-82M ONNX
        std::vector<int64_t> input_shape = {1, (int64_t)tokens.size()};
        std::vector<int64_t> style_shape = {1, 256}; // Style vector
        std::vector<int64_t> speed_shape = {1};      // Speed scalar

        std::vector<float> speed_vec = {speed};

        // Memory Info
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // Prepare Tensors
        std::vector<Ort::Value> input_tensors;
        
        // 1. Input Tokens
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(
            memory_info, const_cast<int64_t*>(tokens.data()), tokens.size(), 
            input_shape.data(), input_shape.size()));

        // 2. Style Vector
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info, current_style_.data(), current_style_.size(), 
            style_shape.data(), style_shape.size()));

        // 3. Speed
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info, speed_vec.data(), speed_vec.size(), 
            speed_shape.data(), speed_shape.size()));

        const char* input_names[] = {"tokens", "style", "speed"};
        const char* output_names[] = {"audio"};

        // Run Inference
        auto output_tensors = session_->Run(
            Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 3, output_names, 1
        );

        // Extract Output
        float* float_audio = output_tensors.front().GetTensorMutableData<float>();
        size_t audio_len = output_tensors.front().GetTensorTypeAndShapeInfo().GetElementCount();

        return std::vector<float>(float_audio, float_audio + audio_len);
    }

private:
    std::unique_ptr<Ort::Env> env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<float> current_style_;
    std::mutex model_mutex_;
    bool is_loaded_ = false;
};

} // namespace stts
