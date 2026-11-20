#pragma once
// Pure DSP microphone coloration: no model IO, ONNX, worker thread or audio
// allocation. One instance per OBS planar channel to preserve IIR history.
#include "framework/dsp/simple_dsp.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stts {
class MicDspEngine {
public:
    enum Profile : int {
        BYPASS = 0, DYNAMIC = 1, CONDENSER = 2, RIBBON = 3,
        TELEPHONE = 4, RADIO = 5, TAPE = 6
    };
    static bool valid(int type) { return type >= BYPASS && type <= TAPE; }

    void configure(Profile type, uint32_t sample_rate) {
        profile_ = type;
        sample_rate_ = std::max<uint32_t>(sample_rate, 8000);
        profile_filters_.clear();
        vibe_filter_.reset();
        // Biquad::reset clears state but not coefficients. A profile switch
        // must also restore neutral EQ instead of inheriting the old sweep.
        vibe_filter_.set_coeffs(dsp::BiquadCoeffs{});
        vibe_knob_.reset(0.5f);
        last_vibe_ = 0.5f;
        gain_ = 1.0f;
        saturation_ = 0.0f;
        control_index_ = 0;
        const float sr = float(sample_rate_);
        using D = dsp::FilterDesign;
        auto add = [&](D::Type mode, float frequency, float q, float db = 0) {
            dsp::Biquad stage;
            const float safe_frequency = std::clamp(frequency, 5.f, sr * .45f);
            stage.set_coeffs(D::design_rbj(mode, safe_frequency, sr, q, db));
            profile_filters_.push_back(stage);
        };
        switch (type) {
        case DYNAMIC:
            add(D::HighPass, 150, .7f);
            add(D::Peaking, 2500, 1.f, 4.f);
            break;
        case CONDENSER:
            add(D::HighPass, 70, .707f);
            add(D::HighShelf, 6200, .707f, 2.f);
            break;
        case RIBBON:
            add(D::HighPass, 65, .707f);
            add(D::LowShelf, 250, .707f, 1.5f);
            add(D::HighShelf, 6500, .707f, -3.f);
            break;
        case TELEPHONE:
            add(D::HighPass, 400, .7f);
            add(D::HighPass, 400, .7f);
            add(D::LowPass, 3000, .7f);
            add(D::LowPass, 3000, .7f);
            gain_ = 3.f;
            saturation_ = .5f;
            break;
        case RADIO:
            add(D::HighPass, 600, 1.f);
            add(D::LowPass, 2500, 1.f);
            gain_ = 1.5f;
            saturation_ = .8f;
            break;
        case TAPE:
            add(D::HighPass, 35, .707f);
            add(D::LowPass, 12000, .707f);
            gain_ = 1.15f;
            saturation_ = .55f;
            break;
        case BYPASS:
            break;
        }
        vibe_knob_.update_coeff(50.f, sr / 16.f);
    }

    void set_parameters(float vibe, float resonance) {
        vibe_knob_.set_target(std::clamp(vibe, 0.f, 1.f));
        const float next_resonance = std::clamp(resonance, .1f, 5.f);
        if (next_resonance != resonance_) {
            resonance_ = next_resonance;
            // Resonance must take effect even if the Vibe slider stays put.
            set_vibe_coeffs(last_vibe_);
        }
    }

    void process(float *samples, size_t count) {
        if (!samples || !count || profile_ == BYPASS) return;
        for (auto &filter : profile_filters_) {
            for (size_t i = 0; i < count; ++i) {
                samples[i] = filter.process(samples[i]);
            }
        }
        for (size_t i = 0; i < count; ++i) {
            if (control_index_++ % 16 == 0) {
                const float vibe = vibe_knob_.get_next();
                if (std::abs(vibe - last_vibe_) > .001f) {
                    last_vibe_ = vibe;
                    set_vibe_coeffs(vibe);
                }
            }
            float sample = vibe_filter_.process(samples[i]) * gain_;
            if (saturation_ > 0.f) {
                sample = std::tanh(sample * (1.f + saturation_));
            }
            samples[i] = sample;
        }
    }

private:
    void set_vibe_coeffs(float vibe) {
        dsp::BiquadCoeffs c; // Identity at neutral.
        using D = dsp::FilterDesign;
        const float sr = float(sample_rate_);
        if (vibe < .48f) {
            const float freq = 200.f * std::pow(100.f, vibe / .48f);
            c = D::design_rbj(D::LowPass,
                std::clamp(freq, 5.f, sr * .45f), sr, resonance_);
        } else if (vibe > .52f) {
            const float freq = 20.f * std::pow(250.f, (vibe - .52f) / .48f);
            c = D::design_rbj(D::HighPass,
                std::clamp(freq, 5.f, sr * .45f), sr, resonance_);
        }
        vibe_filter_.set_coeffs(c);
    }

    Profile profile_ = BYPASS;
    uint32_t sample_rate_ = 48000;
    std::vector<dsp::Biquad> profile_filters_;
    dsp::Biquad vibe_filter_;
    dsp::SmoothedValue vibe_knob_;
    float last_vibe_ = .5f, resonance_ = .707f;
    float gain_ = 1.f, saturation_ = 0.f;
    uint64_t control_index_ = 0;
};
} // namespace stts
