#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/gpu_utils.hpp"
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>

namespace stts {

/**
 * @class NeuCodecEngine
 * @brief Neural audio codec engine using NeuCodec ONNX models.
 * 
 * NeuCodec is a lightweight 0.8kbps codec that:
 * - Encodes audio to FSQ tokens (50 tokens/sec)
 * - Decodes tokens back to audio with upsampling (16kHz → 24kHz)
 * 
 * This serves as a simpler reference implementation compared to RVC,
 * demonstrating how to add new model types to the framework.
 * 
 * Models: https://huggingface.co/neuphonic/neucodec-onnx-decoder
 * 
 * Pipeline:
 * 1. Resample input to 16kHz
 * 2. Encode audio → FSQ codes (using encoder model)
 * 3. Decode FSQ codes → 24kHz audio (using decoder model)
 * 4. Resample output to target sample rate
 */
class NeuCodecEngine : public InferenceEngine {
public:
    NeuCodecEngine() : 
        env_(ORT_LOGGING_LEVEL_WARNING, "NeuCodec_Engine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {}

    virtual bool initialize(const std::string& config) override {
        try {
            // Parse config: encoder_path|decoder_path|threads|backend
            std::stringstream ss(config);
            std::string encoder_path, decoder_path, threads_str, backend_str;
            std::getline(ss, encoder_path, '|');
            std::getline(ss, decoder_path, '|');
            std::getline(ss, threads_str, '|');
            std::getline(ss, backend_str, '|');

            num_threads_ = 2;
            if (!threads_str.empty()) num_threads_ = std::stoi(threads_str);

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

            // At minimum we need the decoder for reconstruction
            ready_ = (decoder_session_ != nullptr);
            std::cout << "[NeuCodec] Initialized with " << num_threads_ << " threads"
                      << (use_gpu_ ? " (GPU)" : " (CPU)") << std::endl;
            return ready_;
        } catch (const std::exception& e) {
            std::cerr << "[NeuCodec] Init error: " << e.what() << std::endl;
            ready_ = false;
            return false;
        }
    }

    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!ready_) return input;

        try {
            // Step 1: Resample to 16kHz (NeuCodec's native input rate)
            std::vector<float> audio_16k = Resampler::resample(input, sample_rate, 16000);
            
            if (audio_16k.empty()) return input;

            std::vector<float> output;

            // Step 2: If we have encoder, encode then decode (full codec loop)
            // Otherwise, just pass through (decoder-only mode requires pre-encoded tokens)
            if (encoder_session_ && decoder_session_) {
                // Encode audio to FSQ codes
                std::vector<int64_t> codes = encode(audio_16k);
                
                // Decode FSQ codes back to audio
                output = decode(codes);
            } else if (decoder_session_) {
                // Decoder-only mode: This would require pre-encoded tokens
                // For now, pass through the audio
                std::cerr << "[NeuCodec] Encoder not loaded, passing through audio." << std::endl;
                output = audio_16k;
            } else {
                return input;
            }

            // Step 3: Resample from 24kHz (NeuCodec output) to target sample rate
            if (!output.empty() && sample_rate != 24000) {
                output = Resampler::resample(output, 24000, sample_rate);
            }

            return output;
        } catch (const std::exception& e) {
            std::cerr << "[NeuCodec] Process error: " << e.what() << std::endl;
            return input;
        }
    }

    virtual std::string get_name() const override { return "NeuCodec (ONNX)"; }
    virtual bool is_ready() const override { return ready_; }

private:
    // =========================================================================
    // Encoder: Audio → FSQ Codes
    // =========================================================================
    std::vector<int64_t> encode(const std::vector<float>& audio) {
        // Input shape: [batch=1, channels=1, samples]
        std::vector<int64_t> shape = {1, 1, static_cast<int64_t>(audio.size())};
        
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, const_cast<float*>(audio.data()), audio.size(),
            shape.data(), shape.size());

        // Run encoder
        std::vector<const char*> input_names = {"audio"};
        std::vector<const char*> output_names = {"codes"};
        
        auto outputs = encoder_session_->Run(
            Ort::RunOptions{nullptr},
            input_names.data(), &input_tensor, 1,
            output_names.data(), output_names.size());

        // Extract codes
        int64_t* data = outputs[0].GetTensorMutableData<int64_t>();
        auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t total = 1;
        for (auto s : out_shape) total *= s;

        return std::vector<int64_t>(data, data + total);
    }

    // =========================================================================
    // Decoder: FSQ Codes → Audio
    // =========================================================================
    std::vector<float> decode(const std::vector<int64_t>& codes) {
        // Input shape: [batch=1, seq_len] or model-specific
        std::vector<int64_t> shape = {1, static_cast<int64_t>(codes.size())};
        
        Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(
            memory_info_, const_cast<int64_t*>(codes.data()), codes.size(),
            shape.data(), shape.size());

        // Run decoder
        std::vector<const char*> input_names = {"codes"};
        std::vector<const char*> output_names = {"audio"};
        
        auto outputs = decoder_session_->Run(
            Ort::RunOptions{nullptr},
            input_names.data(), &input_tensor, 1,
            output_names.data(), output_names.size());

        // Extract audio (24kHz output)
        float* data = outputs[0].GetTensorMutableData<float>();
        auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t total = 1;
        for (auto s : out_shape) total *= s;

        return std::vector<float>(data, data + total);
    }

    // =========================================================================
    // Utilities
    // =========================================================================
    void log_model_info(const std::string& name, Ort::Session* session) {
        if (!session) return;
        
        Ort::AllocatorWithDefaultOptions allocator;
        
        std::cout << "[NeuCodec] " << name << " model loaded. Inputs: ";
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
    std::unique_ptr<Ort::Session> encoder_session_;
    std::unique_ptr<Ort::Session> decoder_session_;
    int num_threads_ = 2;
    bool use_gpu_ = false;
    bool ready_ = false;
};

} // namespace stts
