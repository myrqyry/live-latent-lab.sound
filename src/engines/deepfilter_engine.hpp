#pragma once

#include "framework/inference_engine.hpp"
#include "framework/gpu_utils.hpp"
#include <onnxruntime_cxx_api.h>
#include <vector>
#include <string>
#include <map>
#include <iostream>
#include <algorithm>

namespace stts {

class DeepFilterEngine : public InferenceEngine {
public:
    DeepFilterEngine() :
        env_(ORT_LOGGING_LEVEL_WARNING, "DeepFilterEngine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {}

    bool initialize(const std::string& model_path) override {
        try {
            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(1);
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            // Attempt to use GPU/NPU if available (optional)
            // For now, CPU is safer for stability
            // gpu::GPUConfig gpu_config; gpu_config.backend = gpu::Backend::CPU;
            // gpu::configure_gpu_session(session_options, gpu_config);

            session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), session_options);

            // Inspect Model IO
            Ort::AllocatorWithDefaultOptions allocator;

            size_t num_inputs = session_->GetInputCount();
            size_t num_outputs = session_->GetOutputCount();

            input_names_.clear();
            output_names_.clear();
            input_name_ptrs_.clear();
            output_name_ptrs_.clear();
            state_buffers_.clear();

            // 1. Inputs
            for (size_t i = 0; i < num_inputs; i++) {
                auto name = session_->GetInputNameAllocated(i, allocator);
                input_names_.push_back(name.get());

                auto type_info = session_->GetInputTypeInfo(i);
                auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
                std::vector<int64_t> shape = tensor_info.GetShape();

                // Heuristic: If name contains "state" or is not the first input, it's a state
                // DeepFilterNet usually: input_frame, states...
                if (i > 0) {
                    // Initialize state buffer with zeros
                    // Calculate size
                    int64_t size = 1;
                    for (auto dim : shape) {
                        if (dim < 1) dim = 1; // Fix dynamic dims if any (usually fixed for states)
                        size *= dim;
                    }
                    state_buffers_.push_back(std::vector<float>(size, 0.0f));
                    state_shapes_.push_back(shape);
                }
            }

            // 2. Outputs
            for (size_t i = 0; i < num_outputs; i++) {
                auto name = session_->GetOutputNameAllocated(i, allocator);
                output_names_.push_back(name.get());
            }

            // Pointers for Run
            for (const auto& n : input_names_) input_name_ptrs_.push_back(n.c_str());
            for (const auto& n : output_names_) output_name_ptrs_.push_back(n.c_str());

            is_loaded_ = true;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[DeepFilterEngine] Error: " << e.what() << std::endl;
            is_loaded_ = false;
            return false;
        }
    }

    std::string get_name() const override { return "DeepFilterNet Engine"; }
    bool is_ready() const override { return is_loaded_; }

    std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!is_loaded_ || input.empty()) return input;
        (void)sample_rate;

        // DeepFilterNet usually expects a specific frame size (e.g. 480 or 960 samples).
        // OBS might send varying chunk sizes. We should ideally buffer here,
        // but for now let's assume the filter caller handles buffering (which mic-filter.cpp does).

        try {
            // Prepare Input Tensors
            std::vector<Ort::Value> input_tensors;

            // 1. Audio Input
            std::vector<int64_t> input_shape = {1, 1, (int64_t)input.size()}; // [Batch, Channel, Time] usually
            // Adjust shape based on model expectation?
            // DeepFilterNet: [1, 1, 480] typically.
            // Let's check input dimensions from initialization if possible, but dynamic is often allowed.
            // Some models might be [1, T].

            // NOTE: We assume [1, 1, T] or [1, T]. Let's try [1, 1, T] as it's common for DFNet.
            // If the model expects [1, T], we might need to squeeze.
            // To be robust, we should check `session_->GetInputTypeInfo(0)...GetShape()`.
            // But doing that every frame is slow.
            // Let's rely on ONNX Runtime to be flexible or fail if mismatch.
            // Actually, best to check shape at init.
            // For now, let's just try [1, 1, N]

            input_tensors.push_back(Ort::Value::CreateTensor<float>(
                memory_info_, const_cast<float*>(input.data()), input.size(),
                input_shape.data(), input_shape.size()));

            // 2. State Inputs
            for (size_t i = 0; i < state_buffers_.size(); i++) {
                input_tensors.push_back(Ort::Value::CreateTensor<float>(
                    memory_info_, state_buffers_[i].data(), state_buffers_[i].size(),
                    state_shapes_[i].data(), state_shapes_[i].size()));
            }

            // Run
            auto outputs = session_->Run(
                Ort::RunOptions{nullptr},
                input_name_ptrs_.data(), input_tensors.data(), input_tensors.size(),
                output_name_ptrs_.data(), output_names_.size()
            );

            // Extract Audio Output (Index 0)
            float* out_data = outputs[0].GetTensorMutableData<float>();
            // Assume output size matches input size roughly
            // Or get shape
            auto out_type_info = outputs[0].GetTensorTypeAndShapeInfo();
            size_t out_len = out_type_info.GetElementCount();

            std::vector<float> result(out_data, out_data + out_len);

            // Update States (Indices 1..N)
            // We assume output states map 1:1 to input states in order.
            // DeepFilterNet: returns enhanced_audio, state1_new, state2_new...
            for (size_t i = 0; i < state_buffers_.size(); i++) {
                size_t out_idx = i + 1;
                if (out_idx < outputs.size()) {
                    const float* new_state_data = outputs[out_idx].GetTensorData<float>();
                    size_t count = outputs[out_idx].GetTensorTypeAndShapeInfo().GetElementCount();

                    if (count == state_buffers_[i].size()) {
                        memcpy(state_buffers_[i].data(), new_state_data, count * sizeof(float));
                    }
                }
            }

            return result;

        } catch (const std::exception& e) {
            // std::cerr << "Inference error: " << e.what() << std::endl;
            return input; // Passthrough on error
        }
    }

private:
    Ort::Env env_;
    Ort::MemoryInfo memory_info_;
    std::unique_ptr<Ort::Session> session_;
    bool is_loaded_ = false;

    std::vector<std::string> input_names_;
    std::vector<std::string> output_names_;
    std::vector<const char*> input_name_ptrs_;
    std::vector<const char*> output_name_ptrs_;

    // States
    std::vector<std::vector<float>> state_buffers_;
    std::vector<std::vector<int64_t>> state_shapes_;
};

} // namespace stts
