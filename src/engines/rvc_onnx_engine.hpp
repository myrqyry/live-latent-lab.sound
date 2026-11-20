#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/gpu_utils.hpp"
#include "engines/rvc_model_contract.hpp"
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace stts {

/**
 * @class RvcOnnxEngine
 * @brief Experimental RVC ONNX adapter; real voice conversion remains unverified.
 * 
 * Pipeline:
 * 1. HuBERT (Content Model): Extracts speech features from 16kHz audio
 * 2. RMVPE (Pitch Model): Extracts F0 (fundamental frequency) contour
 * 3. Voice Model (net_g): Combines features + F0 to synthesize target voice
 */
class RvcOnnxEngine : public InferenceEngine {
public:
    RvcOnnxEngine() : 
        env_(ORT_LOGGING_LEVEL_WARNING, "RVC_Engine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)),
        rng_(std::random_device{}())
    {}

    virtual bool initialize(const std::string& config) override {
        ready_ = false;
        voice_session_.reset();
        pitch_session_.reset();
        content_session_.reset();
        try {
            // Parse config: voice_path|pitch_path|content_path|threads|backend|output_rate
            std::stringstream ss(config);
            std::string voice_path, pitch_path, content_path, threads_str, backend_str, rate_str;
            std::getline(ss, voice_path, '|');
            std::getline(ss, pitch_path, '|');
            std::getline(ss, content_path, '|');
            std::getline(ss, threads_str, '|');
            std::getline(ss, backend_str, '|');
            std::getline(ss, rate_str, '|');
            // No inferred output sample rate: RVC exports support several.
            output_sample_rate_ = rate_str.empty() ? 0 : std::stoi(rate_str);
            if (voice_path.empty() || pitch_path.empty() || content_path.empty() ||
                (output_sample_rate_ != 32000 && output_sample_rate_ != 40000 &&
                 output_sample_rate_ != 48000)) {
                ready_ = false;
                return false;
            }

            num_threads_ = 2;
            if (!threads_str.empty()) num_threads_ = std::stoi(threads_str);

            Ort::SessionOptions session_options;
            session_options.SetIntraOpNumThreads(num_threads_);
            
            // Configure GPU if available
            gpu::GPUConfig gpu_config;
            gpu_config.backend = gpu::parse_backend(backend_str.empty() ? "auto" : backend_str);
            use_gpu_ = gpu::configure_gpu_session(session_options, gpu_config);

            // All three graphs are required. Stage and validate the exact IO
            // contracts before publishing a ready engine. A lone voice model
            // is not evidence of functioning conversion.
            auto voice = std::make_unique<Ort::Session>(env_, voice_path.c_str(), session_options);
            auto pitch = std::make_unique<Ort::Session>(env_, pitch_path.c_str(), session_options);
            auto content = std::make_unique<Ort::Session>(env_, content_path.c_str(), session_options);
            std::string reason;
            if (!RvcModelContract::validate(*voice, *pitch, *content, reason)) {
                std::cerr << "[RVC Engine] Incompatible ONNX models: " << reason << std::endl;
                ready_ = false;
                return false;
            }
            voice_session_ = std::move(voice);
            pitch_session_ = std::move(pitch);
            content_session_ = std::move(content);
            log_model_info("Voice", voice_session_.get());
            log_model_info("Pitch", pitch_session_.get());
            log_model_info("Content", content_session_.get());
            detect_model_parameters();
            ready_ = true;
            std::cout << "[RVC Engine] Initialized with " << num_threads_ << " threads"
                      << (use_gpu_ ? " (GPU)" : " (CPU)") << std::endl;
            if (ready_) {
                std::cout << "[RVC Engine] Detected embedding dim: " << embedding_dim_ << std::endl;
            }
            return ready_;
        } catch (const std::exception& e) {
            std::cerr << "[RVC Engine] Init error: " << e.what() << std::endl;
            ready_ = false;
            voice_session_.reset();
            pitch_session_.reset();
            content_session_.reset();
            return false;
        }
    }

    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (!ready_ || !voice_session_ || !pitch_session_ || !content_session_) return input;

        try {
            // Step 1: Resample input to 16kHz for content/pitch models
            std::vector<float> audio_16k = Resampler::resample(input, sample_rate, 16000);
            
            if (audio_16k.empty()) return input;

            // Step 2: Extract content features (HuBERT)
            std::vector<float> hubert_features;
            int64_t feature_length = 0;
            hubert_features = run_hubert(audio_16k, feature_length);

            // Step 3: Extract pitch (F0) using RMVPE
            std::vector<float> f0;
            f0 = run_pitch_extraction(audio_16k);

            // Step 4: Run voice synthesis
            std::vector<float> output;
            if (!hubert_features.empty() && !f0.empty() && feature_length > 0) {
                output = run_voice_synthesis(hubert_features, f0, feature_length, sample_rate);
            }
            // The output must be finite, nonempty and within a bounded
            // duration of the incoming chunk, otherwise preserve dry audio.
            if (output.empty() || output.size() > input.size() * 2 ||
                std::any_of(output.begin(), output.end(),
                    [](float sample) { return !std::isfinite(sample); })) {
                ready_ = false; // A schema match alone is not inference success.
                return input;
            }
            return output;
        } catch (const std::exception& e) {
            std::cerr << "[RVC Engine] Process error: " << e.what() << std::endl;
            ready_ = false;
            return input;
        }
    }

    virtual std::string get_name() const override { return "RVC (ONNX Built-in)"; }
    virtual bool is_ready() const override { return ready_; }

private:
    void detect_model_parameters() {
        if (!voice_session_) return;

        model_input_names_.clear();
        embedding_dim_ = 768; // Default to RVC v2
        Ort::AllocatorWithDefaultOptions allocator;

        size_t count = voice_session_->GetInputCount();
        for (size_t i = 0; i < count; ++i) {
            auto name_ptr = voice_session_->GetInputNameAllocated(i, allocator);
            std::string name = name_ptr.get();
            model_input_names_.push_back(name);

            // Heuristic: check 'phone' input for embedding dimension
            if (name.find("phone") != std::string::npos && name.find("lengths") == std::string::npos) {
                auto type_info = voice_session_->GetInputTypeInfo(i);
                auto shape = type_info.GetTensorTypeAndShapeInfo().GetShape();
                // Standard exported phone shape: [batch, frames, channels].
                if (shape.size() >= 3) {
                    if (shape[2] == 256) embedding_dim_ = 256;
                    else if (shape[2] == 768) embedding_dim_ = 768;
                }
            }
        }

        // If "phone" wasn't explicitly found, try the first input if it looks right
        if (embedding_dim_ == 768 && count > 0) { // Still default
             auto type_info = voice_session_->GetInputTypeInfo(0);
             auto shape = type_info.GetTensorTypeAndShapeInfo().GetShape();
             if (shape.size() >= 3) {
                 if (shape[2] == 256) embedding_dim_ = 256;
             }
        }
    }

    bool has_input(const std::string& name) const {
        return std::find(model_input_names_.begin(), model_input_names_.end(), name) != model_input_names_.end();
    }

    // =========================================================================
    // HuBERT Content Feature Extraction
    // =========================================================================
    std::vector<float> run_hubert(const std::vector<float>& audio, int64_t& out_length) {
        // Input: source [batch=1, seq_len] and padding_mask [batch=1, seq_len]
        std::vector<int64_t> audio_shape = {1, static_cast<int64_t>(audio.size())};
        
        Ort::Value audio_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, const_cast<float*>(audio.data()), audio.size(), 
            audio_shape.data(), audio_shape.size());

        // Create padding mask (all 0 = all valid audio)
        // Note: Using int64_t instead of bool because std::vector<bool> has no .data() method
        std::vector<uint8_t> padding_mask(audio.size(), 0);
        Ort::Value mask_tensor = Ort::Value::CreateTensor(
            memory_info_, padding_mask.data(), padding_mask.size(),
            audio_shape.data(), audio_shape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);

        // Run inference
        std::vector<const char*> input_names = {"source", "padding_mask"};
        std::vector<const char*> output_names = {"embed"};
        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(audio_tensor));
        inputs.push_back(std::move(mask_tensor));

        auto outputs = content_session_->Run(
            Ort::RunOptions{nullptr},
            input_names.data(), inputs.data(), inputs.size(),
            output_names.data(), output_names.size());

        // Extract features [batch, seq, embedding_dim]
        auto& output_tensor = outputs[0];
        auto shape = output_tensor.GetTensorTypeAndShapeInfo().GetShape();
        out_length = shape.size() > 1 ? shape[1] : 0;
        
        float* data = output_tensor.GetTensorMutableData<float>();
        size_t total = 1;
        for (auto s : shape) total *= s;
        
        return std::vector<float>(data, data + total);
    }

    // =========================================================================
    // RMVPE Pitch (F0) Extraction
    // =========================================================================
    std::vector<float> run_pitch_extraction(const std::vector<float>& audio) {
        // RMVPE expects audio at 16kHz
        std::vector<int64_t> shape = {1, static_cast<int64_t>(audio.size())};
        
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info_, const_cast<float*>(audio.data()), audio.size(),
            shape.data(), shape.size());

        std::vector<const char*> input_names = {"audio"};
        std::vector<const char*> output_names = {"pitchf"};
        
        auto outputs = pitch_session_->Run(
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
    // Voice Synthesis (net_g)
    // =========================================================================
    std::vector<float> run_voice_synthesis(
        const std::vector<float>& hubert_features,
        const std::vector<float>& f0,
        int64_t feature_length,
        uint32_t target_sample_rate
    ) {
        int64_t batch = 1;
        const size_t channels = static_cast<size_t>(embedding_dim_);
        if (hubert_features.size() != static_cast<size_t>(feature_length) * channels ||
            f0.empty()) return {};
        const size_t frames = std::min<size_t>(static_cast<size_t>(feature_length) * 2, f0.size());
        if (frames < 1) return {};
        // Source embeds 50 frames/s; RVC net_g consumes ~100 frames/s.
        // Interpolate along the time axis without transposing the channels.
        std::vector<float> phone(frames * channels);
        for (size_t t = 0; t < frames; ++t) {
            const size_t left = t / 2;
            const size_t right = std::min<size_t>(left + 1, feature_length - 1);
            const float fraction = (t % 2) * 0.5f;
            for (size_t c = 0; c < channels; ++c)
                phone[t * channels + c] =
                    hubert_features[left * channels + c] * (1 - fraction) +
                    hubert_features[right * channels + c] * fraction;
        }
        feature_length = static_cast<int64_t>(frames);
        std::vector<int64_t> hubert_shape = {batch, feature_length, embedding_dim_};
        
        // Prepare data vectors
        std::vector<int64_t> length_data = {feature_length};
        std::vector<int64_t> length_shape = {batch};
        std::vector<int64_t> f0_shape = {batch, feature_length};
        std::vector<float> continuous_pitch(f0.begin(), f0.begin() + frames);
        std::vector<int64_t> coarse_pitch(frames);
        const double mel_min = 1127.0 * std::log(1.0 + 50.0 / 700.0);
        const double mel_max = 1127.0 * std::log(1.0 + 1100.0 / 700.0);
        for (size_t i = 0; i < frames; ++i) {
            const double f = std::max(0.0, double(continuous_pitch[i]));
            const double mel = 1127.0 * std::log(1.0 + f / 700.0);
            const double value = f > 0 ? ((mel - mel_min) * 254.0 /
                (mel_max - mel_min) + 1.0) : 1.0;
            coarse_pitch[i] = static_cast<int64_t>(std::lround(std::clamp(value, 1.0, 255.0)));
        }
        std::vector<int64_t> ds_data = {0};
        std::vector<int64_t> ds_shape = {batch};
        
        // Random noise
        std::vector<float> rnd(192 * static_cast<size_t>(feature_length));
        std::normal_distribution<float> dist(0.0f, 1.0f);
        for (auto& r : rnd) r = dist(rng_);
        std::vector<int64_t> rnd_shape = {batch, 192, feature_length};

        // Inputs collection
        std::vector<const char*> input_names_ptr;
        std::vector<Ort::Value> inputs;

        // Note: The input names here must match what the model expects.
        // We add them conditionally based on what we detected or assume standard names if present.

        if (has_input("phone")) {
            inputs.push_back(Ort::Value::CreateTensor<float>(
                memory_info_, phone.data(), phone.size(),
                hubert_shape.data(), hubert_shape.size()));
            input_names_ptr.push_back("phone");
        }

        if (has_input("phone_lengths")) {
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memory_info_, length_data.data(), length_data.size(),
                length_shape.data(), length_shape.size()));
            input_names_ptr.push_back("phone_lengths");
        }

        if (has_input("pitch")) {
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memory_info_, coarse_pitch.data(), coarse_pitch.size(),
                f0_shape.data(), f0_shape.size()));
            input_names_ptr.push_back("pitch");
        }

        if (has_input("pitchf")) {
             inputs.push_back(Ort::Value::CreateTensor<float>(
                memory_info_, continuous_pitch.data(), continuous_pitch.size(),
                f0_shape.data(), f0_shape.size()));
            input_names_ptr.push_back("pitchf");
        }

        if (has_input("ds")) {
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memory_info_, ds_data.data(), ds_data.size(),
                ds_shape.data(), ds_shape.size()));
            input_names_ptr.push_back("ds");
        }

        if (has_input("rnd")) {
             inputs.push_back(Ort::Value::CreateTensor<float>(
                memory_info_, rnd.data(), rnd.size(),
                rnd_shape.data(), rnd_shape.size()));
            input_names_ptr.push_back("rnd");
        }
        
        std::vector<const char*> output_names = {"audio"};

        auto outputs = voice_session_->Run(
            Ort::RunOptions{nullptr},
            input_names_ptr.data(), inputs.data(), inputs.size(),
            output_names.data(), output_names.size());

        // Extract output audio
        float* data = outputs[0].GetTensorMutableData<float>();
        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t total = 1;
        for (auto s : shape) total *= s;

        std::vector<float> output_audio(data, data + total);

        const uint32_t rvc_output_rate = static_cast<uint32_t>(output_sample_rate_);
        if (target_sample_rate != rvc_output_rate) {
            output_audio = Resampler::resample(output_audio, rvc_output_rate, target_sample_rate);
        }

        return output_audio;
    }

    // =========================================================================
    // Utilities
    // =========================================================================
    void log_model_info(const std::string& name, Ort::Session* session) {
        if (!session) return;
        
        Ort::AllocatorWithDefaultOptions allocator;
        
        std::cout << "[RVC Engine] " << name << " model loaded. Inputs: ";
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
    std::unique_ptr<Ort::Session> voice_session_;
    std::unique_ptr<Ort::Session> pitch_session_;
    std::unique_ptr<Ort::Session> content_session_;
    std::mt19937 rng_;
    int num_threads_ = 2;
    int output_sample_rate_ = 0;
    bool use_gpu_ = false;
    bool ready_ = false;

    // Model parameters
    int64_t embedding_dim_ = 768;
    std::vector<std::string> model_input_names_;
};

} // namespace stts
