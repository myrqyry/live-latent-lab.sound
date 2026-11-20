#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/gpu_utils.hpp"
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <cmath>
#include <array>

namespace stts {

/**
 * @class DemucsEngine
 * @brief Audio source separation engine using Demucs/HTDemucs ONNX models.
 * 
 * Demucs v4 (Hybrid Transformer Demucs) separates audio into stems:
 * - Vocals
 * - Drums  
 * - Bass
 * - Other (everything else)
 * 
 * Can also use 6-stem model: vocals, drums, bass, guitar, piano, other
 * 
 * Pipeline:
 * 1. Process stereo audio at 44.1kHz
 * 2. Run through Demucs model
 * 3. Output selected stem(s)
 * 
 * Model Sources:
 * - Intel OpenVINO: https://huggingface.co/Intel/demucs-openvino
 * - ONNX C++: https://github.com/sevagh/demucs.onnx
 * - Official: https://github.com/facebookresearch/demucs
 * 
 * Note: HTDemucs requires STFT/iSTFT operations which may need to be
 * handled outside the ONNX model for compatibility.
 */
class DemucsEngine : public InferenceEngine {
public:
    // Stem indices
    enum class Stem {
        DRUMS = 0,
        BASS = 1,
        OTHER = 2,
        VOCALS = 3
    };

    DemucsEngine() : 
        env_(ORT_LOGGING_LEVEL_WARNING, "Demucs_Engine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {}

    virtual bool initialize(const std::string& config) override {
        try {
            // Parse config: model_path|stem_to_output|threads|backend
            // stem_to_output: "vocals", "drums", "bass", "other", "instrumental", "all"
            // backend: "auto", "cuda", "cpu", "directml"
            std::stringstream ss(config);
            std::string model_path, stem_str, threads_str, backend_str;
            std::getline(ss, model_path, '|');
            std::getline(ss, stem_str, '|');
            std::getline(ss, threads_str, '|');
            std::getline(ss, backend_str, '|');

            num_threads_ = 2;
            if (!threads_str.empty()) num_threads_ = std::stoi(threads_str);
            
            // Parse stem selection
            output_stem_ = parse_stem(stem_str.empty() ? "vocals" : stem_str);

            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(num_threads_);
            
            // Configure GPU if available
            gpu::GPUConfig gpu_config;
            gpu_config.backend = gpu::parse_backend(backend_str.empty() ? "auto" : backend_str);
            use_gpu_ = gpu::configure_gpu_session(session_options, gpu_config);

            // Load model
            if (!model_path.empty()) {
                model_session_ = std::make_unique<Ort::Session>(env_, model_path.c_str(), session_options);
                log_model_info("Demucs", model_session_.get());
            }

            ready_ = (model_session_ != nullptr);
            std::cout << "[Demucs] Initialized with " << num_threads_ << " threads"
                      << (use_gpu_ ? " (GPU)" : " (CPU)") 
                      << ", output: " << stem_str << std::endl;
            return ready_;
        } catch (const std::exception& e) {
            std::cerr << "[Demucs] Init error: " << e.what() << std::endl;
            ready_ = false;
            return false;
        }
    }

    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!ready_ || !model_session_) return input;

        try {
            // Step 1: Resample to 44.1kHz if needed
            std::vector<float> audio_44k = input;
            if (sample_rate != 44100) {
                audio_44k = Resampler::resample(input, sample_rate, 44100);
            }
            
            // Step 2: Ensure we have enough samples (Demucs typically processes in chunks)
            // Minimum chunk size is usually around 10 seconds for quality
            const size_t min_samples = 44100 * 2; // 2 seconds minimum
            if (audio_44k.size() < min_samples) {
                // Pad with zeros
                audio_44k.resize(min_samples, 0.0f);
            }
            
            // Step 3: Run separation
            auto stems = separate(audio_44k);
            
            // Step 4: Select output based on configuration
            std::vector<float> output;
            
            if (output_instrumental_) {
                // Instrumental = drums + bass + other (everything except vocals)
                output = mix_stems(stems, {Stem::DRUMS, Stem::BASS, Stem::OTHER});
            } else if (output_all_) {
                // All stems (for visualization/debugging)
                output = stems[0]; // Just return first stem for now
            } else {
                // Single stem output
                int stem_idx = static_cast<int>(output_stem_);
                if (stem_idx < static_cast<int>(stems.size())) {
                    output = stems[stem_idx];
                } else {
                    output = audio_44k;
                }
            }
            
            // Step 5: Resample back to original sample rate
            if (sample_rate != 44100 && !output.empty()) {
                output = Resampler::resample(output, 44100, sample_rate);
            }
            
            // Ensure output matches input length
            output.resize(input.size(), 0.0f);
            
            return output;
        } catch (const std::exception& e) {
            std::cerr << "[Demucs] Process error: " << e.what() << std::endl;
            return input;
        }
    }

    virtual std::string get_name() const override { return "Demucs (Source Separation)"; }
    virtual bool is_ready() const override { return ready_; }

    // Configuration methods
    void set_output_stem(Stem stem) { 
        output_stem_ = stem; 
        output_instrumental_ = false;
        output_all_ = false;
    }
    
    void set_output_instrumental(bool enable) { 
        output_instrumental_ = enable; 
        output_all_ = false;
    }

    /**
     * Process audio and return all stems separately.
     * Returns vector of 4 stems: [drums, bass, other, vocals]
     * Each stem has the same number of samples as input.
     */
    virtual std::vector<std::vector<float>> process_all_stems(const std::vector<float>& input, uint32_t sample_rate) override {
        std::vector<std::vector<float>> result(4);
        
        if (!ready_ || !model_session_) {
            // Never invent four separated stems from a missing model.
            return {};
        }

        try {
            // Resample to 44.1kHz if needed
            std::vector<float> audio_44k = input;
            if (sample_rate != 44100) {
                audio_44k = Resampler::resample(input, sample_rate, 44100);
            }
            
            // Ensure minimum samples
            const size_t min_samples = 44100 * 2;
            if (audio_44k.size() < min_samples) {
                audio_44k.resize(min_samples, 0.0f);
            }
            
            // Run separation. Two-stem or malformed exports must never be
            // promoted to four supposedly independent outputs.
            auto stems = separate(audio_44k);
            if (stems.size() != STEM_COUNT)
                throw std::runtime_error("Demucs returned an incompatible stem count");
            for (const auto& stem : stems) {
                if (stem.empty())
                    throw std::runtime_error("Demucs returned an empty stem");
            }

            // Resample back to original sample rate
            for (size_t i = 0; i < stems.size(); i++) {
                if (sample_rate != 44100) {
                    result[i] = Resampler::resample(stems[i], 44100, sample_rate);
                } else {
                    result[i] = stems[i];
                }
                result[i].resize(input.size(), 0.0f);
            }
            
            return result;
        } catch (const std::exception& e) {
            std::cerr << "[Demucs] Process all stems error: " << e.what() << std::endl;
            // Failure is silence to stem consumers, not fabricated separation.
            return {};
        }
    }
    
    // Get stem count
    static constexpr int STEM_COUNT = 4;

private:
    // =========================================================================
    // Source Separation
    // =========================================================================
    std::vector<std::vector<float>> separate(const std::vector<float>& audio) {
        // Input shape: [batch=1, channels=2, samples]
        // For mono input, duplicate to stereo
        size_t num_samples = audio.size();
        std::vector<float> stereo_audio(num_samples * 2);
        for (size_t i = 0; i < num_samples; i++) {
            stereo_audio[i] = audio[i];                    // Left
            stereo_audio[num_samples + i] = audio[i];      // Right (duplicate mono)
        }
        
        std::vector<int64_t> shape = {1, 2, static_cast<int64_t>(num_samples)};
        
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, stereo_audio.data(), stereo_audio.size(),
            shape.data(), shape.size());

        // Run inference
        std::vector<const char*> input_names = {"audio"};
        std::vector<const char*> output_names = {"sources"};
        
        auto outputs = model_session_->Run(
            Ort::RunOptions{nullptr},
            input_names.data(), &input_tensor, 1,
            output_names.data(), output_names.size());

        // Parse output: [batch=1, sources=4, channels=2, samples]
        float* data = outputs[0].GetTensorMutableData<float>();
        auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        
        int num_sources = (out_shape.size() > 1) ? static_cast<int>(out_shape[1]) : 4;
        int num_channels = (out_shape.size() > 2) ? static_cast<int>(out_shape[2]) : 2;
        int out_samples = (out_shape.size() > 3) ? static_cast<int>(out_shape[3]) : static_cast<int>(num_samples);
        
        // Extract stems (convert stereo to mono by averaging)
        std::vector<std::vector<float>> stems(num_sources);
        for (int s = 0; s < num_sources; s++) {
            stems[s].resize(out_samples);
            for (int i = 0; i < out_samples; i++) {
                float left = data[s * num_channels * out_samples + i];
                float right = data[s * num_channels * out_samples + out_samples + i];
                stems[s][i] = (left + right) * 0.5f;
            }
        }
        
        return stems;
    }

    std::vector<float> mix_stems(const std::vector<std::vector<float>>& stems, 
                                  std::initializer_list<Stem> to_mix) {
        if (stems.empty()) return {};
        
        size_t num_samples = stems[0].size();
        std::vector<float> output(num_samples, 0.0f);
        
        for (Stem s : to_mix) {
            int idx = static_cast<int>(s);
            if (idx < static_cast<int>(stems.size())) {
                for (size_t i = 0; i < num_samples; i++) {
                    output[i] += stems[idx][i];
                }
            }
        }
        
        // Normalize to prevent clipping
        float max_val = 0.0f;
        for (const auto& v : output) {
            max_val = std::max(max_val, std::abs(v));
        }
        if (max_val > 1.0f) {
            for (auto& v : output) {
                v /= max_val;
            }
        }
        
        return output;
    }

    Stem parse_stem(const std::string& str) {
        if (str == "drums") return Stem::DRUMS;
        if (str == "bass") return Stem::BASS;
        if (str == "other") return Stem::OTHER;
        if (str == "vocals") return Stem::VOCALS;
        if (str == "instrumental") {
            output_instrumental_ = true;
            return Stem::VOCALS; // Will be ignored
        }
        if (str == "all") {
            output_all_ = true;
            return Stem::VOCALS; // Will be ignored
        }
        return Stem::VOCALS; // Default
    }

    // =========================================================================
    // Utilities
    // =========================================================================
    void log_model_info(const std::string& name, Ort::Session* session) {
        if (!session) return;
        
        Ort::AllocatorWithDefaultOptions allocator;
        
        std::cout << "[Demucs] " << name << " model loaded. Inputs: ";
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

    // Member variables
    Ort::Env env_;
    Ort::MemoryInfo memory_info_;
    std::unique_ptr<Ort::Session> model_session_;
    Stem output_stem_ = Stem::VOCALS;
    bool output_instrumental_ = false;
    bool output_all_ = false;
    int num_threads_ = 2;
    bool use_gpu_ = false;
    bool ready_ = false;
};

} // namespace stts
