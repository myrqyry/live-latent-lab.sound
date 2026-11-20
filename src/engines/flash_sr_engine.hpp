#pragma once

#include "framework/inference_engine.hpp"
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <onnxruntime_cxx_api.h>

namespace stts {

class FlashSREngine : public InferenceEngine {
public:
    FlashSREngine() {}

    // Initialize the engine with the .onnx model path
    bool initialize(const std::string& model_path) override {
        std::lock_guard<std::mutex> lock(model_mutex_);
        try {
            // Configure ONNX Session
            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "FlashSREngine");
            session_options_.SetIntraOpNumThreads(1);
            session_options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            // Append CUDA provider if available (FlashSR benefits heavily from GPU)
            // OrtSessionOptionsAppendExecutionProvider_CUDA(session_options_, 0);

            session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options_);
            is_loaded_ = true;
            return true;
        } catch (const Ort::Exception& e) {
            is_loaded_ = false;
            return false;
        }
    }

    std::string get_name() const override { return "FlashSR Audio Upscaler"; }
    bool is_ready() const override { return is_loaded_; }

    // Standard process used by generic filters, but we likely need specific generate/process for SR
    std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
         return process_sr(input, sample_rate);
    }
    
    /**
     * @brief Upsample audio from 24kHz to 48kHz using FlashSR.
     */
    std::vector<float> process_sr(const std::vector<float>& input_24k, uint32_t /*input_rate*/) {
        if (!is_loaded_ || input_24k.empty()) return input_24k;

        std::lock_guard<std::mutex> lock(model_mutex_);
        
        // FlashSR typically expects [Batch, 1, Samples]
        // And potentially a "steps" parameter for diffusion.
        
        std::vector<int64_t> input_shape = {1, 1, (int64_t)input_24k.size()};
        
        // Memory Info
        auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // Inputs
        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info, const_cast<float*>(input_24k.data()), input_24k.size(), 
            input_shape.data(), input_shape.size()));
            
        // Optional: Steps (if model exports it as input). We assume standard 1-step or fixed step graph for now.

        const char* input_names[] = {"audio_in"};
        const char* output_names[] = {"audio_out"};

        try {
            auto output_tensors = session_->Run(
                Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 1, output_names, 1
            );
            
            float* out_data = output_tensors.front().GetTensorMutableData<float>();
            size_t count = output_tensors.front().GetTensorTypeAndShapeInfo().GetElementCount();
            
            return std::vector<float>(out_data, out_data + count);
        } catch (...) {
            return input_24k; // Fallback to original if inference fails
        }
    }

private:
    std::unique_ptr<Ort::Env> env_;
    Ort::SessionOptions session_options_;
    std::unique_ptr<Ort::Session> session_;
    std::mutex model_mutex_;
    bool is_loaded_ = false;
};

} // namespace stts
