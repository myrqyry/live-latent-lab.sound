#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/gpu_utils.hpp"
#include <demucs.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <mutex>

namespace stts {

/**
 * @class DemucsCppEngine
 * @brief High-performance Demucs engine using the demucs.onnx logic.
 * 
 * This engine handles STFT/iSTFT in C++ and uses optimized ORT inference.
 * It is based on https://github.com/sevagh/demucs.onnx
 */
class DemucsCppEngine : public InferenceEngine {
public:
    enum class Stem {
        DRUMS = 0,
        BASS = 1,
        OTHER = 2,
        VOCALS = 3
    };

    DemucsCppEngine() = default;
    
    virtual ~DemucsCppEngine() = default;

    virtual bool initialize(const std::string& config) override {
        try {
            // Parse config: model_path|stem_to_output|threads|backend
            std::stringstream ss(config);
            std::string model_path, stem_str, threads_str, backend_str;
            std::getline(ss, model_path, '|');
            std::getline(ss, stem_str, '|');
            std::getline(ss, threads_str, '|');
            std::getline(ss, backend_str, '|');

            int threads = 4;
            if (!threads_str.empty()) threads = std::stoi(threads_str);

            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(threads);
            
            // GPU
            gpu::GPUConfig gpu_config;
            gpu_config.backend = gpu::parse_backend(backend_str.empty() ? "auto" : backend_str);
            gpu::configure_gpu_session(session_options, gpu_config);

            // Load model using demucsonnx API
            // Read file into vector
            std::ifstream file(model_path, std::ios::binary);
            if (!file) {
                blog(LOG_ERROR, "[DemucsCpp] Failed to open model: %s", model_path.c_str());
                return false;
            }
            std::vector<char> model_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            
            if (!demucsonnx::load_model(model_data, model_, session_options)) {
                blog(LOG_ERROR, "[DemucsCpp] Failed to load model structure");
                return false;
            }

            ready_ = true;
            return true;
        } catch (const std::exception& e) {
            blog(LOG_ERROR, "[DemucsCpp] Init error: %s", e.what());
            return false;
        }
    }

    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!ready_) return input;

        // Process all stems and return selected
        auto stems = process_all_stems(input, sample_rate);
        
        // Default to vocals (index 3)
        if (stems.size() > 3) return stems[3];
        return input;
    }

    virtual std::vector<std::vector<float>> process_all_stems(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!ready_ || input.empty()) return {};

        try {
            // Resample to 44.1kHz (required by demucs.onnx)
            std::vector<float> audio_44k = input;
            if (sample_rate != 44100) {
                audio_44k = Resampler::resample(input, sample_rate, 44100);
            }

            // Convert to Eigen Matrix (Stereo)
            // demucs.onnx expects 2 channels
            Eigen::MatrixXf eigen_audio(2, audio_44k.size());
            for (size_t i = 0; i < audio_44k.size(); i++) {
                eigen_audio(0, i) = audio_44k[i];
                eigen_audio(1, i) = audio_44k[i]; // Duplicate mono
            }

            // Run inference
            auto progress_cb = [](float /*progress*/, const std::string& /*msg*/) {
                // blog(LOG_DEBUG, "[DemucsCpp] %s: %.1f%%", msg.c_str(), progress * 100.0f);
            };

            Eigen::Tensor3dXf result = demucsonnx::demucs_inference(model_, eigen_audio, progress_cb);

            // result shape: [sources, channels, samples]
            int nb_sources = result.dimension(0);
            int nb_samples = result.dimension(2);

            std::vector<std::vector<float>> stems(nb_sources);
            for (int s = 0; s < nb_sources; s++) {
                stems[s].resize(nb_samples);
                for (int i = 0; i < nb_samples; i++) {
                    // Average stereo to mono
                    stems[s][i] = (result(s, 0, i) + result(s, 1, i)) * 0.5f;
                }
                
                // Resample back if needed
                if (sample_rate != 44100) {
                    stems[s] = Resampler::resample(stems[s], 44100, sample_rate);
                }
                
                // Ensure length matches input
                stems[s].resize(input.size(), 0.0f);
            }

            return stems;
        } catch (const std::exception& e) {
            blog(LOG_ERROR, "[DemucsCpp] Process error: %s", e.what());
            return {};
        }
    }

    virtual std::string get_name() const override { return "HTDemucs (Advanced C++)"; }
    virtual bool is_ready() const override { return ready_; }

private:
    demucsonnx::demucs_model model_;
    bool ready_ = false;
};

} // namespace stts
