#pragma once

#include "framework/inference_engine.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <deque>
#include <algorithm>
#include <mutex>

#ifdef USE_LITERT
#include <tensorflow/lite/c/c_api.h>
#endif

namespace stts {

class YamnetEngine : public InferenceEngine {
public:
    YamnetEngine() {
#ifdef USE_LITERT
        // Yamnet specific constants
        // Input: 15600 samples @ 16kHz (~0.975s)
        input_size_ = 15600;
        target_sr_ = 16000;
#endif
    }

    ~YamnetEngine() {
#ifdef USE_LITERT
        if (interpreter_) TfLiteInterpreterDelete(interpreter_);
        if (model_) TfLiteModelDelete(model_);
        if (options_) TfLiteInterpreterOptionsDelete(options_);
#endif
    }

    virtual bool initialize(const std::string& config) override {
#ifdef USE_LITERT
        // Config: model_path|labels_path
        size_t delim = config.find('|');
        std::string model_path = config;
        std::string labels_path;
        if (delim != std::string::npos) {
            model_path = config.substr(0, delim);
            labels_path = config.substr(delim + 1);
        }

        try {
            model_ = TfLiteModelCreateFromFile(model_path.c_str());
            if (!model_) {
                std::cerr << "[Yamnet] Failed to load model: " << model_path << std::endl;
                return false;
            }

            options_ = TfLiteInterpreterOptionsCreate();
            TfLiteInterpreterOptionsSetNumThreads(options_, 2);

            interpreter_ = TfLiteInterpreterCreate(model_, options_);
            if (!interpreter_) {
                std::cerr << "[Yamnet] Failed to create interpreter" << std::endl;
                return false;
            }

            if (TfLiteInterpreterAllocateTensors(interpreter_) != kTfLiteOk) {
                std::cerr << "[Yamnet] Failed to allocate tensors" << std::endl;
                return false;
            }

            // Load labels
            std::ifstream file(labels_path);
            if (file.is_open()) {
                std::string line;
                while (std::getline(file, line)) {
                    labels_.push_back(line);
                }
                file.close();
            } else {
                std::cerr << "[Yamnet] Warning: Could not load labels from " << labels_path << std::endl;
            }

            ready_ = true;
            std::cout << "[Yamnet] Initialized (Labels: " << labels_.size() << ")" << std::endl;
            return true;
        } catch (...) {
            return false;
        }
#else
        std::cerr << "[Yamnet] LiteRT not compiled." << std::endl;
        return false;
#endif
    }

    // Process generic audio chunk (expected 48k or whatever pipeline sends)
    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!ready_) return input;

        std::lock_guard<std::mutex> lock(buffer_mutex_);

        // 1. Resample to 16kHz logic (Primitive linear interpolation for now or assume pipeline sends 48k)
        // Ideally we use a proper resampler, but for classification simple decimation might suffice if sr is multiple
        // Let's implement a basic ratio resampler.
        
        float ratio = (float)target_sr_ / sample_rate;
        float pos = 0.0f;
        
        while (pos < input.size()) {
            int idx = (int)pos;
            if (idx < input.size()) {
                audio_buffer_.push_back(input[idx]);
                // Limit buffer size to input_size_ (rolling window)
                if (audio_buffer_.size() > input_size_) {
                     audio_buffer_.pop_front();
                }
            }
            pos += 1.0f / ratio;
        }

        return input; // Return passthrough
    }

    // Return top classification result
    std::pair<std::string, float> classify() {
#ifdef USE_LITERT
        if (!ready_) return {"Not Ready", 0.0f};

        std::unique_lock<std::mutex> lock(buffer_mutex_);
        if (audio_buffer_.size() < input_size_) return { "Buffering...", 0.0f };

        // Copy buffer to vector
        std::vector<float> input_vec(audio_buffer_.begin(), audio_buffer_.end());
        lock.unlock();

        // Prepare Tensor
        TfLiteTensor* input_tensor = TfLiteInterpreterGetInputTensor(interpreter_, 0);
        TfLiteTensorCopyFromBuffer(input_tensor, input_vec.data(), input_vec.size() * sizeof(float));

        // Inference
        if (TfLiteInterpreterInvoke(interpreter_) != kTfLiteOk) {
            return { "Error", 0.0f };
        }

        // Output Index 0 is Scores [1, 521]
        const TfLiteTensor* output_tensor = TfLiteInterpreterGetOutputTensor(interpreter_, 0);
        float* scores = (float*)TfLiteTensorData(output_tensor);
        int num_classes = labels_.size(); // Should match 521 for Yamnet

        int best_idx = -1;
        float best_score = -1.0f;

        for (int i = 0; i < num_classes; i++) {
             if (scores[i] > best_score) {
                 best_score = scores[i];
                 best_idx = i;
             }
        }

        if (best_idx >= 0 && best_idx < labels_.size()) {
            return { labels_[best_idx], best_score };
        }
        return { "Unknown", 0.0f };
#else
        return { "No Backend", 0.0f };
#endif
    }

    virtual std::string get_name() const override { return "Yamnet (LiteRT)"; }
    virtual bool is_ready() const override { return ready_; }

private:
#ifdef USE_LITERT
    TfLiteModel* model_ = nullptr;
    TfLiteInterpreter* interpreter_ = nullptr;
    TfLiteInterpreterOptions* options_ = nullptr;
    size_t input_size_ = 0;
    int target_sr_ = 16000;
#endif
    bool ready_ = false;
    std::deque<float> audio_buffer_;
    std::mutex buffer_mutex_;
    std::vector<std::string> labels_;
};

} // namespace stts
