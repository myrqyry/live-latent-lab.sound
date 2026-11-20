#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/gpu_utils.hpp"
#include "framework/dsp/simple_dsp.hpp"
#include <onnxruntime_cxx_api.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <map>
#include <random>

namespace stts {

/**
 * @class MicEngine
 * @brief Live Latent Vibe Station Engine
 *
 * Upgraded spectral processing engine.
 * Features:
 * - Neural Mic Modeling (ONNX)
 * - Analog-style Tone Shaping (DSPFilters)
 * - Smooth "DJ Vibe" Filter Sweeps
 */
class MicEngine : public InferenceEngine {
public:
    enum class MicType {
        NONE = 0,
        GENERIC_DYNAMIC = 1, 
        GENERIC_CONDENSER = 2,
        RIBBON = 3,
        TELEPHONE = 4,
        VINTAGE_RADIO = 5,
        TAPE_SATURATION = 6,
        CUSTOM_ONNX = 7
    };

    MicEngine() :
        env_(ORT_LOGGING_LEVEL_WARNING, "Mic_Engine"),
        memory_info_(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        reset_filters();
    }

    virtual bool initialize(const std::string& config) override {
        // config: mic_type|model_path|backend
        std::stringstream ss(config);
        std::string type_str, path, backend_str;
        std::getline(ss, type_str, '|');
        std::getline(ss, path, '|');
        std::getline(ss, backend_str, '|');

        int type_int = 0;
        if (!type_str.empty()) type_int = std::stoi(type_str);
        current_mic_ = static_cast<MicType>(type_int);

        // Reset
        reset_filters();
        setup_dsp_chain(current_mic_);

        // Do not advertise a missing model as a ready neural processor.
        if (current_mic_ == MicType::CUSTOM_ONNX && path.empty()) {
            session_.reset();
            ready_ = false;
            return false;
        }

        // Load ONNX
        if (current_mic_ == MicType::CUSTOM_ONNX) {
            try {
                Ort::SessionOptions session_options;
                session_options.SetIntraOpNumThreads(2);

                gpu::GPUConfig gpu_config;
                gpu_config.backend = gpu::parse_backend(backend_str.empty() ? "auto" : backend_str);
                bool use_gpu = gpu::configure_gpu_session(session_options, gpu_config);

                session_ = std::make_unique<Ort::Session>(env_, path.c_str(), session_options);
                // The current runtime supports exactly one float32 [1, N]
                // input named "input" and one float32 [1, N] output named
                // "output". Reject any other graph before the live audio
                // thread receives it; never claim arbitrary ONNX support.
                Ort::AllocatorWithDefaultOptions allocator;
                if (session_->GetInputCount() != 1 || session_->GetOutputCount() != 1)
                    throw std::runtime_error("Custom mic model must have exactly one input and output");
                auto input_name = session_->GetInputNameAllocated(0, allocator);
                auto output_name = session_->GetOutputNameAllocated(0, allocator);
                if (std::string(input_name.get()) != "input" ||
                    std::string(output_name.get()) != "output")
                    throw std::runtime_error("Custom mic model requires input/output tensor names");
                auto in_info = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
                auto out_info = session_->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
                auto in_shape = in_info.GetShape();
                auto out_shape = out_info.GetShape();
                if (in_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                    out_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                    in_shape.size() != 2 || out_shape.size() != 2 ||
                    (in_shape[0] != 1 && in_shape[0] != -1) ||
                    (out_shape[0] != 1 && out_shape[0] != -1) ||
                    (in_shape[1] != 1024 && in_shape[1] >= 0) ||
                    (out_shape[1] != 1024 && out_shape[1] >= 0))
                    throw std::runtime_error("Custom mic model must accept/return float32 [1,1024]");
                ready_ = true;
                std::cout << "[Vibe Engine] Loaded custom model: " << path << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "[Vibe Engine] Error loading model: " << e.what() << std::endl;
                ready_ = false;
                session_.reset();
                current_mic_ = MicType::NONE;
            }
        } else {
            ready_ = true;
            session_ = nullptr;
        }
        return ready_;
    }

    // Setters for Vibe Station Parameters
    void set_vibe(float val) { vibe_knob_.set_target(val); } // 0.0 - 1.0 (0.5 neutral)
    void set_resonance(float val) { resonance_ = val; } 

    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        if (input.empty() || current_mic_ == MicType::NONE || !ready_) return input;
        if (session_ && current_mic_ == MicType::CUSTOM_ONNX)
            return process_onnx(input, sample_rate);
        std::vector<float> output = input;
        process_dsp_inplace(output.data(), output.size(), sample_rate);
        return output;
    }

    // Zero-allocation DSP entry point for OBS's real-time audio callback.
    // Each filter instance processes only one planar channel (independent IIR state).
    void process_dsp_inplace(float* output, size_t count, uint32_t sample_rate) {
        if (!output || !count || !ready_ || current_mic_ == MicType::NONE ||
            current_mic_ == MicType::CUSTOM_ONNX) return;

        if (sample_rate_ != sample_rate) {
            sample_rate_ = sample_rate;
            setup_dsp_chain(current_mic_);
        }
        // Parameter update occurs every 16 samples, so coefficient timing is
        // calculated at the actual control rate, not the audio sample rate.
        vibe_knob_.update_coeff(50.0f, (float)sample_rate / 16.0f);

        for (auto& filter : profile_filters_) {
            for (size_t i = 0; i < count; ++i)
                output[i] = filter.process(output[i]);
        }

        float current_vibe = vibe_knob_.get();
        for (size_t i = 0; i < count; ++i) {
            if (i % 16 == 0) {
                float v = vibe_knob_.get_next();
                if (std::abs(v - current_vibe) > 0.001f) {
                    current_vibe = v;
                    update_vibe_coeffs(v);
                }
            }
            output[i] = vibe_filter_.process(output[i]);
        }

        if (saturation_amount_ > 0.0f || gain_ != 1.0f) {
            for (size_t i = 0; i < count; ++i) {
                output[i] *= gain_;
                if (saturation_amount_ > 0.0f)
                    output[i] = std::tanh(output[i] * (1.0f + saturation_amount_));
            }
        }
    }

    virtual std::string get_name() const override { return "Vibe Station"; }
    virtual bool is_ready() const override { return ready_; }

private:
    MicType current_mic_ = MicType::NONE;
    uint32_t sample_rate_ = 44100;
    bool ready_ = false;

    // AI
    Ort::Env env_;
    Ort::MemoryInfo memory_info_;
    std::unique_ptr<Ort::Session> session_;

    // DSP Chains
    std::vector<dsp::Biquad> profile_filters_;
    
    // Vibe Filter (Dynamic DJ Filter)
    dsp::Biquad vibe_filter_;
    dsp::SmoothedValue vibe_knob_; // 0.0 (LowPass) -> 0.5 (Neutral) -> 1.0 (HighPass)
    float resonance_ = 0.707f; // Q factor

    float saturation_amount_ = 0.0f;
    float gain_ = 1.0f;

    void reset_filters() {
        profile_filters_.clear();
        vibe_filter_.reset();
        saturation_amount_ = 0.0f;
        gain_ = 1.0f;
        
        // Initialize vibe neutral
        vibe_knob_.reset(0.5f);
    }

    void setup_dsp_chain(MicType type) {
        // Re-implement profiles using new DSP wrapper
        using namespace dsp;
        profile_filters_.clear();

        switch (type) {
            case MicType::GENERIC_DYNAMIC:
                // High pass 150Hz
                {
                   Biquad bq; bq.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighPass, 150.0f, sample_rate_, 0.7f));
                   profile_filters_.push_back(bq);
                }
                // Presence Boost
                {
                   Biquad bq; bq.set_coeffs(FilterDesign::design_rbj(FilterDesign::Peaking, 2500.0f, sample_rate_, 1.0f, 4.0f));
                   profile_filters_.push_back(bq);
                }
                break;
            case MicType::GENERIC_CONDENSER: {
                // Wide bandwidth with a restrained presence lift.
                Biquad hp, air;
                hp.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighPass, 70.0f, sample_rate_, 0.707f));
                air.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighShelf, 6200.0f, sample_rate_, 0.707f, 2.0f));
                profile_filters_.push_back(hp);
                profile_filters_.push_back(air);
                break;
            }
            case MicType::RIBBON: {
                // Warm low-mid response with attenuated high frequencies.
                Biquad hp, warmth, rolloff;
                hp.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighPass, 65.0f, sample_rate_, 0.707f));
                warmth.set_coeffs(FilterDesign::design_rbj(FilterDesign::LowShelf, 250.0f, sample_rate_, 0.707f, 1.5f));
                rolloff.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighShelf, 6500.0f, sample_rate_, 0.707f, -3.0f));
                profile_filters_.push_back(hp);
                profile_filters_.push_back(warmth);
                profile_filters_.push_back(rolloff);
                break;
            }
            case MicType::TAPE_SATURATION: {
                // Gentle bandwidth limits followed by non-linear saturation.
                Biquad hp, lp;
                hp.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighPass, 35.0f, sample_rate_, 0.707f));
                lp.set_coeffs(FilterDesign::design_rbj(FilterDesign::LowPass,
                    std::min(12000.0f, float(sample_rate_) * 0.42f), sample_rate_, 0.707f));
                profile_filters_.push_back(hp);
                profile_filters_.push_back(lp);
                saturation_amount_ = 0.55f;
                gain_ = 1.15f;
                break;
            }
            case MicType::TELEPHONE:
                {
                   // Simulating steep bandpass with cascaded filters
                   Biquad hp1, hp2, lp1, lp2;
                   hp1.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighPass, 400.0f, sample_rate_, 0.7f));
                   hp2.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighPass, 400.0f, sample_rate_, 0.7f)); // Cascade for steeper slope
                   lp1.set_coeffs(FilterDesign::design_rbj(FilterDesign::LowPass, 3000.0f, sample_rate_, 0.7f));
                   lp2.set_coeffs(FilterDesign::design_rbj(FilterDesign::LowPass, 3000.0f, sample_rate_, 0.7f));

                   profile_filters_.push_back(hp1); profile_filters_.push_back(hp2);
                   profile_filters_.push_back(lp1); profile_filters_.push_back(lp2);
                   
                   saturation_amount_ = 0.5f;
                   gain_ = 3.0f;
                }
                break;
            case MicType::VINTAGE_RADIO:
                 // Bandpass 600 - 2500
                {
                   Biquad hp, lp;
                   hp.set_coeffs(FilterDesign::design_rbj(FilterDesign::HighPass, 600.0f, sample_rate_, 1.0f));
                   lp.set_coeffs(FilterDesign::design_rbj(FilterDesign::LowPass, 2500.0f, sample_rate_, 1.0f));
                   profile_filters_.push_back(hp); profile_filters_.push_back(lp);
                   saturation_amount_ = 0.8f;
                   gain_ = 1.5f;
                }
                break;
             // Add others as needed...
            default: break;
        }
    }

    void update_vibe_coeffs(float vibe) {
        // Vibe: 0.0 (LowPass) ... 0.5 (Neutral) ... 1.0 (HighPass)
        // Map 0.0-0.45 -> LP Freq 200Hz - 20kHz
        // Map 0.55-1.0 -> HP Freq 20Hz - 5kHz
        // Center: Neutral (Bypass)

        using namespace dsp;
        BiquadCoeffs c; // Default Identity

        if (vibe < 0.48f) {
            // Low Pass Sweep
            // 0.0 -> 200Hz, 0.48 -> 20000Hz
            // Logarithmic sweep feels better?
            // Linear map:
            float t = vibe / 0.48f; 
            float freq = 200.0f * std::pow(100.0f, t); // 200 * 100^1 = 20k
            if (freq > 20000.0f) freq = 20000.0f;
            
            c = FilterDesign::design_rbj(FilterDesign::LowPass, freq, sample_rate_, resonance_);
        } 
        else if (vibe > 0.52f) {
            // High Pass Sweep
            // 0.52 -> 20Hz, 1.0 -> 5000Hz?
            float t = (vibe - 0.52f) / 0.48f;
            float freq = 20.0f * std::pow(250.0f, t); // 20 * 250 = 5000
            
            c = FilterDesign::design_rbj(FilterDesign::HighPass, freq, sample_rate_, resonance_);
        }
        else {
            // Neutral - Flat
            // RBJ doesn't have "Pass" but we can set identity manually or use wide filter
            // b0=1, all else 0 is set by default constructor of BiquadCoeffs? No.
            // design_rbj returns normalized.
            // Let's use internal bypass flag or just a dummy filter (Wide peaking 0dB)
             c.b0=1; c.b1=0; c.b2=0; c.a1=0; c.a2=0; 
        }
        vibe_filter_.set_coeffs(c);
    }

    // The custom ONNX interface is intentionally limited to checked mono I/O.
    std::vector<float> process_onnx(const std::vector<float>& input, uint32_t sample_rate) {
        (void)sample_rate;
        try {
            std::vector<int64_t> shape = {1, static_cast<int64_t>(input.size())};
            Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
                memory_info_, const_cast<float*>(input.data()), input.size(),
                shape.data(), shape.size());
            const char* input_names[] = {"input"};
            const char* output_names[] = {"output"};
            auto outputs = session_->Run(
                Ort::RunOptions{nullptr},
                input_names, &input_tensor, 1,
                output_names, 1);
            float* data = outputs[0].GetTensorMutableData<float>();
            auto out_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
            size_t total = 1; for (auto s : out_shape) total *= s;
            return std::vector<float>(data, data + total);
        } catch (...) { return input; }
    }
};

} // namespace stts
