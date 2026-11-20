#pragma once
#include <vector>
#include <cmath>
#include <complex>
#include <numbers>

namespace stts {
namespace dsp {

// ============================================================================
// Parameter Smoothing (One-Pole Lowpass)
// ============================================================================
class SmoothedValue {
public:
    void reset(float value) {
        current_ = value;
        target_ = value;
    }

    void set_target(float target) {
        target_ = target;
    }

    // Call this once per sample processing block or per sample
    // smoothing_time_ms: approx time to reach 99% of target
    void update_coeff(float smoothing_time_ms, float sample_rate) {
        if (smoothing_time_ms <= 0.0f) {
            coeff_ = 0.0f; // Instant
        } else {
            // formula: coeff = exp(-1 / (time * rate)) ? 
            // Better: simple exponential decay
            // y[n] = y[n-1] + (x[n] - y[n-1]) * alpha
            // alpha = 1 - exp(-2pi * cutoff / rate)?
            // Let's use simple RC time constant approx:
            // coeff = exp(-params / (ms * rate))
            float samples = (smoothing_time_ms / 1000.0f) * sample_rate;
            if (samples < 1.0f) coeff_ = 0.0f;
            else coeff_ = std::exp(-1.0f / samples); // Typical one-pole formulation
        }
    }

    float get_next() {
        if (std::abs(current_ - target_) < 1e-5f) {
            current_ = target_;
        } else {
            // y[n] = coeff * y[n-1] + (1 - coeff) * target
            // Note: This is a low-pass filter on the PARAMETER
            current_ = (coeff_ * current_) + ((1.0f - coeff_) * target_);
        }
        return current_;
    }

    float get() const { return current_; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float coeff_ = 0.0f;
};

// ============================================================================
// Biquad Filter (Direct Form I - Better for floating point)
// ============================================================================
// Coeffs standard: b0, b1, b2, a0, a1, a2 (normalized by a0)
struct BiquadCoeffs {
    float b0=1, b1=0, b2=0, a1=0, a2=0;
};

class Biquad {
public:
    void reset() {
        x1_ = x2_ = y1_ = y2_ = 0.0f;
    }

    void set_coeffs(const BiquadCoeffs& c) {
        coeffs_ = c;
    }

    // Direct Form I
    // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
    // Normalized a0=1
    inline float process(float in) {
        float out = coeffs_.b0 * in + coeffs_.b1 * x1_ + coeffs_.b2 * x2_ 
                                    - coeffs_.a1 * y1_ - coeffs_.a2 * y2_;
        
        // Denormal protection ? Usually handled by CPU flags, but explicit is safe
        // out += 1e-20f; out -= 1e-20f; 

        x2_ = x1_;
        x1_ = in;
        y2_ = y1_;
        y1_ = out;
        
        return out;
    }

private:
    BiquadCoeffs coeffs_;
    float x1_=0, x2_=0, y1_=0, y2_=0;
};

// ============================================================================
// Filter Designer (RBJ & Basic High Order approximations)
// ============================================================================
class FilterDesign {
public:
    enum Type {
        LowPass, HighPass, BandPass, Notch, Peaking, LowShelf, HighShelf
    };

    static BiquadCoeffs design_rbj(Type type, float freq, float sample_rate, float Q, float dbGain = 0.0f) {
        float w0 = 2.0f * 3.14159265f * freq / sample_rate;
        float cos_w0 = std::cos(w0);
        float sin_w0 = std::sin(w0);
        float alpha = sin_w0 / (2.0f * Q);
        float A = std::pow(10.0f, dbGain / 40.0f);

        float b0, b1, b2, a0, a1, a2;

        switch (type) {
            case LowPass:
                b0 = (1 - cos_w0) / 2;
                b1 = 1 - cos_w0;
                b2 = (1 - cos_w0) / 2;
                a0 = 1 + alpha;
                a1 = -2 * cos_w0;
                a2 = 1 - alpha;
                break;
            case HighPass:
                b0 = (1 + cos_w0) / 2;
                b1 = -(1 + cos_w0);
                b2 = (1 + cos_w0) / 2;
                a0 = 1 + alpha;
                a1 = -2 * cos_w0;
                a2 = 1 - alpha;
                break;
            case BandPass:
                b0 = alpha;
                b1 = 0;
                b2 = -alpha;
                a0 = 1 + alpha;
                a1 = -2 * cos_w0;
                a2 = 1 - alpha;
                break;
            case Peaking:
                b0 = 1 + alpha * A;
                b1 = -2 * cos_w0;
                b2 = 1 - alpha * A;
                a0 = 1 + alpha / A;
                a1 = -2 * cos_w0;
                a2 = 1 - alpha / A;
                break;
            case LowShelf: {
                // RBJ low shelf (slope S=1); Q does not apply to shelf slope.
                float shelf_alpha = sin_w0 * 0.70710678118f;
                float root = 2.0f * std::sqrt(A) * shelf_alpha;
                b0 = A * ((A + 1) - (A - 1) * cos_w0 + root);
                b1 = 2 * A * ((A - 1) - (A + 1) * cos_w0);
                b2 = A * ((A + 1) - (A - 1) * cos_w0 - root);
                a0 = (A + 1) + (A - 1) * cos_w0 + root;
                a1 = -2 * ((A - 1) + (A + 1) * cos_w0);
                a2 = (A + 1) + (A - 1) * cos_w0 - root;
                break;
            }
            case HighShelf: {
                float shelf_alpha = sin_w0 * 0.70710678118f;
                float root = 2.0f * std::sqrt(A) * shelf_alpha;
                b0 = A * ((A + 1) + (A - 1) * cos_w0 + root);
                b1 = -2 * A * ((A - 1) + (A + 1) * cos_w0);
                b2 = A * ((A + 1) + (A - 1) * cos_w0 - root);
                a0 = (A + 1) - (A - 1) * cos_w0 + root;
                a1 = 2 * ((A - 1) - (A + 1) * cos_w0);
                a2 = (A + 1) - (A - 1) * cos_w0 - root;
                break;
            }
            default:
                b0=1; b1=0; b2=0; a0=1; a1=0; a2=0;
        }

        BiquadCoeffs c;
        c.b0 = b0/a0; c.b1 = b1/a0; c.b2 = b2/a0;
        c.a1 = a1/a0; c.a2 = a2/a0;
        return c;
    }
};

} // namespace dsp
} // namespace stts
