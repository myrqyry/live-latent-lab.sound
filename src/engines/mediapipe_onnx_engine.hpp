#pragma once
#include "framework/vision_engine.hpp"
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace stts {

class MediapipeOnnxEngine : public VisionEngine {
public:
    MediapipeOnnxEngine() = default;
    
    virtual ~MediapipeOnnxEngine() = default;

    virtual bool initialize(const std::string& config) override {
        try {
            // parsing config: model_path|backend|threads
            // Simple string stream parsing
            std::string model_path = config;
            size_t delim = config.find('|');
            if (delim != std::string::npos) {
                model_path = config.substr(0, delim);
            }

            env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "MediapipeOnnx");
            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(4);
            session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            // Create session
            session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options);

            // Cache input/output names
            Ort::AllocatorWithDefaultOptions allocator;
            size_t num_input_nodes = session_->GetInputCount();
            for(size_t i = 0; i < num_input_nodes; i++) {
                input_names_.push_back(session_->GetInputNameAllocated(i, allocator).get());
                input_names_ptrs_.push_back(input_names_.back().c_str());
            }

            size_t num_output_nodes = session_->GetOutputCount();
            for(size_t i = 0; i < num_output_nodes; i++) {
                output_names_.push_back(session_->GetOutputNameAllocated(i, allocator).get());
                output_names_ptrs_.push_back(output_names_.back().c_str());
            }

            // Assume standard FaceMesh input of 192x192
             auto input_info = session_->GetInputTypeInfo(0);
             auto input_tensor_info = input_info.GetTensorTypeAndShapeInfo();
             auto shape = input_tensor_info.GetShape();
             
             // Check if symbolic dims (-1)
             if (shape.size() >= 4) {
                 input_width_ = shape[3] > 0 ? shape[3] : 192;
                 input_height_ = shape[2] > 0 ? shape[2] : 192;
             }

            ready_ = true;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[MediapipeOnnx] Init failed: " << e.what() << std::endl;
            return false;
        }
    }

    virtual VisionResult process(const ImageFrame& frame) override {
        VisionResult result;
        if (!ready_ || !session_) return result;

        try {
            // 1. Resize and Normalize Image
            // Target: 1x3xHxW, Planar RGB, -1 to 1 or 0 to 1 depending on model. 
            // Most MP models are [0, 1] or [-1, 1]. Let's try [0, 1] first or check metadata.
            // FaceMesh is typically float32 [0, 1].

            std::vector<float> input_tensor_values(1 * 3 * input_height_ * input_width_);
            
            // Simple Nearest Neighbor Resize + CHW conversion
            float scale_x = (float)frame.width / input_width_;
            float scale_y = (float)frame.height / input_height_;
            
            for (int y = 0; y < input_height_; y++) {
                for (int x = 0; x < input_width_; x++) {
                    // Source coords
                    int src_x = (int)(x * scale_x);
                    int src_y = (int)(y * scale_y);
                    src_x = std::min(src_x, (int)frame.width - 1);
                    src_y = std::min(src_y, (int)frame.height - 1);
                    
                    int src_idx = (src_y * frame.row_bytes) + (src_x * 4); // BGRA
                    
                    uint8_t b = frame.data[src_idx + 0];
                    uint8_t g = frame.data[src_idx + 1];
                    uint8_t r = frame.data[src_idx + 2];
                    
                    // Normalize to [0, 1]
                    float rf = r / 255.0f;
                    float gf = g / 255.0f;
                    float bf = b / 255.0f;
                    
                    // CHW layout
                    // 0: R, 1: G, 2: B
                    input_tensor_values[0 * input_height_ * input_width_ + y * input_width_ + x] = rf;
                    input_tensor_values[1 * input_height_ * input_width_ + y * input_width_ + x] = gf;
                    input_tensor_values[2 * input_height_ * input_width_ + y * input_width_ + x] = bf;
                }
            }

            std::vector<int64_t> input_shape = {1, 3, (int64_t)input_height_, (int64_t)input_width_};
            
            auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, input_tensor_values.data(), input_tensor_values.size(), input_shape.data(), input_shape.size());

            auto output_tensors = session_->Run(Ort::RunOptions{nullptr}, input_names_ptrs_.data(), &input_tensor, 1, output_names_ptrs_.data(), output_names_ptrs_.size());
            
            // Parse Output
            // Face Mesh output [0] is usually 1x1404 (468 points * 3 coords x,y,z)
            // or 1x468x3
            
            if (!output_tensors.empty()) {
                const float* output_data = output_tensors[0].GetTensorData<float>();
                auto info = output_tensors[0].GetTensorTypeAndShapeInfo();
                size_t size = info.GetElementCount();
                
                // Assuming layout x,y,z flattened
                // Number of landmarks = size / 3
                size_t num_landmarks = size / 3;
                
                result.has_detection = true;
                result.score = 1.0f; // Mock score if detector not separated
                
                for (size_t i = 0; i < num_landmarks; i++) {
                    float lx = output_data[i * 3 + 0]; // x
                    float ly = output_data[i * 3 + 1]; // y
                    // float lz = output_data[i * 3 + 2]; // z (ignored for overlay)
                    
                    // Mediapipe coordinates are usually normalized [0, 1] unless raw model output
                    // Some basic models output raw coords relative to 192x192.
                    // If range is > 1, normalize it.
                    if (lx > 1.0f) lx /= input_width_;
                    if (ly > 1.0f) ly /= input_height_;
                    
                    result.landmarks.push_back({lx, ly});
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "[MediapipeOnnx] Inference error: " << e.what() << std::endl;
        }

        return result;
    }

    virtual std::string get_name() const override { return "MediaPipe ONNX (FaceMesh)"; }
    virtual bool is_ready() const override { return ready_; }

private:
    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;
    bool ready_ = false;

    std::vector<std::string> input_names_;
    std::vector<const char*> input_names_ptrs_;
    std::vector<std::string> output_names_;
    std::vector<const char*> output_names_ptrs_;

    int input_width_ = 192;
    int input_height_ = 192;
};

} // namespace stts
