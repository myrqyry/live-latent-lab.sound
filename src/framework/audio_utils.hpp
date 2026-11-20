#pragma once
#include <vector>
#include <cmath>
#include <stdint.h>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace stts {

/**
 * @class Resampler
 * @brief Audio resampling utilities for sample rate conversion.
 * 
 * For production use, consider integrating libsamplerate (Secret Rabbit Code)
 * or Speex resampler for higher quality.
 */
class Resampler {
public:
    /**
     * Resample audio using linear interpolation.
     * Fast but may introduce aliasing artifacts at extreme ratios.
     */
    // It is recommended to use resample_sinc for better quality, though it is slower.
    // Consider replacing this with a library like libsamplerate for production use.
    static std::vector<float> resample(const std::vector<float>& input, uint32_t from_rate, uint32_t to_rate) {
        if (from_rate == to_rate || input.empty()) return input;
        return resample_sinc(input, from_rate, to_rate);
    }

    /**
     * Resample using sinc interpolation (higher quality, slower).
     * Uses a windowed sinc filter for better anti-aliasing.
     */
    static std::vector<float> resample_sinc(const std::vector<float>& input, uint32_t from_rate, uint32_t to_rate, int filter_width = 16) {
        if (from_rate == to_rate || input.empty()) return input;

        double ratio = static_cast<double>(to_rate) / static_cast<double>(from_rate);
        size_t out_samples = static_cast<size_t>(input.size() * ratio);
        std::vector<float> output(out_samples, 0.0f);

        // When downsampling (ratio < 1.0) we must low-pass to avoid aliasing.
        // scale <= 1.0 reduces the cutoff and stretches the sinc in input domain.
        double scale = std::min(1.0, ratio);

        // Effective filter width in input samples. Increase when scale < 1.
        int effective_width = static_cast<int>(std::ceil(filter_width / scale));

        for (size_t i = 0; i < out_samples; i++) {
            double src_pos = static_cast<double>(i) / ratio;
            int src_int = static_cast<int>(std::floor(src_pos));

            int start_j = src_int - effective_width;
            int end_j = src_int + effective_width + 1; // exclusive

            start_j = std::max(0, start_j);
            end_j = std::min(static_cast<int>(input.size()), end_j);

            double sample = 0.0;

            for (int j = start_j; j < end_j; j++) {
                // x is normalized so that 1.0 corresponds to first sinc zero-crossing
                double x = (src_pos - j) * scale;

                if (std::abs(x) >= filter_width) continue;

                double sinc_val = (x == 0.0) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
                // Hann window reduces ringing
                double window = 0.5 * (1.0 + std::cos(M_PI * x / filter_width));

                double weight = sinc_val * window * scale;

                sample += input[j] * weight;
            }

            output[i] = static_cast<float>(sample);
        }

        return output;
    }
};

/**
 * @class AudioNormalizer
 * @brief Audio normalization and preprocessing utilities.
 */
class AudioNormalizer {
public:
    /**
     * Normalize audio to peak amplitude of 1.0
     */
    static void normalize_peak(std::vector<float>& audio) {
        if (audio.empty()) return;
        
        float max_val = 0.0f;
        for (const auto& s : audio) {
            max_val = std::max(max_val, std::abs(s));
        }
        
        if (max_val > 0.0001f) {
            float scale = 1.0f / max_val;
            for (auto& s : audio) {
                s *= scale;
            }
        }
    }

    /**
     * Apply soft clipping to prevent harsh distortion
     */
    static void soft_clip(std::vector<float>& audio, float threshold = 0.9f) {
        for (auto& s : audio) {
            float sign = (s >= 0.0f) ? 1.0f : -1.0f;
            float abs_s = std::abs(s);
            if (abs_s > threshold) {
                float overshoot = abs_s - threshold;
                float knee = 1.0f - threshold;
                s = sign * (threshold + knee * std::tanh(overshoot / knee));
            }
        }
    }

    /**
     * Convert stereo to mono by averaging channels
     */
    static std::vector<float> stereo_to_mono(const float* left, const float* right, size_t frames) {
        std::vector<float> mono(frames);
        for (size_t i = 0; i < frames; i++) {
            mono[i] = (left[i] + right[i]) * 0.5f;
        }
        return mono;
    }
    
    /**
     * @brief Safely calculate buffer size in samples, preventing integer overflow.
     */
    static size_t safe_buffer_size(uint32_t sample_rate, uint32_t duration_sec) {
        uint64_t size_64 = (uint64_t)sample_rate * (uint64_t)duration_sec;
        // Clamp to a reasonable max (e.g. 100MB of float audio ~ 25M samples) to prevent insane allocations
        if (size_64 > 25000000) size_64 = 25000000;
        return (size_t)size_64;
    }
};

} // namespace stts
