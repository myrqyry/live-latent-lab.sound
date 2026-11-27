#pragma once
#include "framework/inference_engine.hpp"
#include "framework/audio_utils.hpp"
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <cstring>
#include <array>
#include <atomic>
#include <cstdint>

namespace stts {

/**
 * @class MatcheringEngine
 * @brief Matches the RMS and EQ curve of a reference audio file.
 */
class MatcheringEngine : public InferenceEngine {
public:
    MatcheringEngine() {}

    struct Parameters {
        float smoothing = 0.5f; // 0.0 (fast) to 1.0 (slow)
        float eq_amount = 1.0f; // 0.0 to 1.0
        float mix = 1.0f;       // 0.0 to 1.0
    };

    struct ReferenceProfile {
        bool ready = false;
        float rms = 0.1f;
        std::array<float,3> energy{0.1f,0.1f,0.1f};
    };

    ReferenceProfile reference_profile() const {
        return {ready_, ref_rms_, {ref_energy_[0],ref_energy_[1],ref_energy_[2]}};
    }

    // Only the audio owner applies a prepared profile to the running DSP.
    void apply_reference(const ReferenceProfile& p) {
        ready_ = p.ready;
        ref_rms_ = p.rms;
        std::copy(p.energy.begin(),p.energy.end(),ref_energy_);
        if (ready_) setup_filters();
    }

    virtual bool initialize(const std::string& config) override {
        // config is just path for now
        reference_path_ = config;

        if (reference_path_.empty()) {
            ready_ = false;
            return false;
        }

        if (load_reference(reference_path_)) {
            analyze_reference();
            setup_filters();
            ready_ = true;
        } else {
            ready_ = false;
        }

        return ready_;
    }

    // Default process not used for multi-channel aware filter, but kept for interface
    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) override {
        // Mono-only fallback
        if (!ready_ || input.empty()) return input;

        // Update state with single channel
        update_analysis(input, sample_rate);

        std::vector<float> output = input;
        process_channel(output, 0); // Use filter set 0
        return output;
    }

    /**
     * @brief Analyze input audio (usually a mono downmix) to update dynamic parameters.
     */
    void update_analysis(const std::vector<float>& input, uint32_t sample_rate) {
        update_analysis(input.data(), nullptr, input.size(), sample_rate);
    }

    // Analyze planar mono/stereo directly, without materializing a downmix.
    void update_analysis(const float* left, const float* right, size_t frames, uint32_t sample_rate) {
        if (!ready_ || !left || !frames || !sample_rate) return;

        if (sample_rate_ != sample_rate) {
            sample_rate_ = sample_rate;
            setup_filters();
        }

        // Compute the same block RMS and band energies from the mono mix.
        Biquad lp = lp_proto_; lp.reset();
        Biquad hp = hp_proto_; hp.reset();
        float sum = 0, sum_low = 0, sum_mid = 0, sum_high = 0;
        for (size_t i=0; i<frames; ++i) {
            const float x = right ? (left[i]+right[i])*0.5f : left[i];
            const float l = lp.process(x), h = hp.process(x), m = x-l-h;
            sum += x*x; sum_low += l*l; sum_mid += m*m; sum_high += h*h;
        }
        const float current_rms = std::sqrt(sum/frames);
        const float in_low = std::sqrt(sum_low/frames);
        const float in_mid = std::sqrt(sum_mid/frames);
        const float in_high = std::sqrt(sum_high/frames);

        // 3. Smooth the measurements
        // Map 0.0-1.0 param to a coefficient
        float alpha = 0.05f * (1.0f - params_.smoothing) + 0.001f * params_.smoothing;

        input_rms_ = input_rms_ * (1.0f - alpha) + current_rms * alpha;
        input_energy_[0] = input_energy_[0] * (1.0f - alpha) + in_low * alpha;
        input_energy_[1] = input_energy_[1] * (1.0f - alpha) + in_mid * alpha;
        input_energy_[2] = input_energy_[2] * (1.0f - alpha) + in_high * alpha;

        // 4. Calculate Target EQ Gains
        float g_low = (input_energy_[0] > 1e-6f) ? (ref_energy_[0] / input_energy_[0]) : 1.0f;
        float g_mid = (input_energy_[1] > 1e-6f) ? (ref_energy_[1] / input_energy_[1]) : 1.0f;
        float g_high = (input_energy_[2] > 1e-6f) ? (ref_energy_[2] / input_energy_[2]) : 1.0f;

        // Apply EQ Amount (Interpolate towards 1.0)
        g_low = 1.0f + (g_low - 1.0f) * params_.eq_amount;
        g_mid = 1.0f + (g_mid - 1.0f) * params_.eq_amount;
        g_high = 1.0f + (g_high - 1.0f) * params_.eq_amount;

        // Clamp EQ gains
        target_gains_[0] = std::clamp(g_low, 0.25f, 4.0f);
        target_gains_[1] = std::clamp(g_mid, 0.25f, 4.0f);
        target_gains_[2] = std::clamp(g_high, 0.25f, 4.0f);

        // 5. Calculate Makeup Gain
        // We estimate the output RMS if we applied these gains.
        // A better way is to measure the actual output RMS, but that requires feedback or a lookahead.
        // We will use the simplified estimation but corrected:
        // Weighted average based on band energy contribution?
        // Approximation: New_RMS ~ Old_RMS * (Energy_Weighted_Avg_Gain)
        float total_energy = input_energy_[0] + input_energy_[1] + input_energy_[2];
        float weighted_gain = 1.0f;
        if (total_energy > 1e-6f) {
            weighted_gain = (input_energy_[0] * target_gains_[0] +
                             input_energy_[1] * target_gains_[1] +
                             input_energy_[2] * target_gains_[2]) / total_energy;
        }

        float predicted_rms = input_rms_ * weighted_gain;
        float makeup = (predicted_rms > 1e-6f) ? (ref_rms_ / predicted_rms) : 1.0f;

        target_makeup_ = std::clamp(makeup, 0.1f, 10.0f);
    }

    /**
     * @brief Process a single channel of audio using the current state.
     * @param channel_idx Used to maintain separate filter states for stereo channels.
     */
    void process_channel(std::vector<float>& buffer, int channel_idx) {
        process_channel(buffer.data(), buffer.size(), channel_idx);
    }

    void process_channel(float* buffer, size_t frames, int channel_idx) {
        if (!ready_ || !buffer || !frames || channel_idx < 0 || channel_idx >= 2) return;
        update_filters_for_gains(target_gains_[0], target_gains_[1], target_gains_[2], channel_idx);
        for (size_t i=0; i<frames; ++i) {
            const float dry = buffer[i];
            float wet = eq_low_[channel_idx].process(dry);
            wet = eq_mid_[channel_idx].process(wet);
            wet = eq_high_[channel_idx].process(wet);
            wet *= target_makeup_;
            constexpr float threshold = 0.98f;
            const float magnitude = std::abs(wet);
            if (magnitude > threshold) {
                constexpr float knee = 1.0f-threshold;
                wet = (wet >= 0.f ? 1.f : -1.f) *
                      (threshold+knee*std::tanh((magnitude-threshold)/knee));
            }
            buffer[i] = params_.mix < 1.f ? dry*(1.f-params_.mix)+wet*params_.mix : wet;
        }
    }

    void set_parameters(const Parameters& p) {
        params_ = p;
    }

    virtual std::string get_name() const override { return "Matchering Engine"; }
    virtual bool is_ready() const override { return ready_; }

private:
    std::string reference_path_;
    bool ready_ = false;
    uint32_t sample_rate_ = 44100;
    Parameters params_;

    // Reference stats
    float ref_rms_ = 0.1f;
    float ref_energy_[3] = {0.1f, 0.1f, 0.1f}; // Low, Mid, High

    // Runtime state (Smoothed)
    float input_rms_ = 0.0f;
    float input_energy_[3] = {0.0f, 0.0f, 0.0f};
    float target_gains_[3] = {1.0f, 1.0f, 1.0f};
    float target_makeup_ = 1.0f;

    // Filters
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float z1 = 0, z2 = 0;
        float process(float in) {
            float out = in * b0 + z1;
            z1 = in * b1 + z2 - a1 * out;
            z2 = in * b2 - a2 * out;
            return out;
        }
        void reset() { z1 = 0; z2 = 0; }
    };

    // 2 sets of EQ filters for Stereo (L=0, R=1)
    Biquad eq_low_[2], eq_mid_[2], eq_high_[2];

    // Analysis filters (Stateless for measurement, but we keep an instance to init coeffs)
    Biquad lp_proto_, hp_proto_;

    bool load_reference(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        // Read header
        char buffer[12];
        if (!file.read(buffer, 12)) return false;

        if (strncmp(buffer, "RIFF", 4) != 0 || strncmp(buffer + 8, "WAVE", 4) != 0) {
            return false;
        }

        // Search for fmt and data chunks
        int channels = 0;
        int sample_rate = 0;
        int bits_per_sample = 0;
        size_t data_size = 0;
        bool found_fmt = false;
        bool found_data = false;

        while (file.read(buffer, 8)) {
            uint32_t chunk_size = *(uint32_t*)(buffer + 4);

            if (strncmp(buffer, "fmt ", 4) == 0) {
                char fmt_data[16]; // Minimal read
                file.read(fmt_data, 16);
                channels = *(uint16_t*)(fmt_data + 2);
                sample_rate = *(uint32_t*)(fmt_data + 4);
                bits_per_sample = *(uint16_t*)(fmt_data + 14);
                found_fmt = true;

                // Skip remaining if any
                if (chunk_size > 16) file.seekg(chunk_size - 16, std::ios::cur);
            } else if (strncmp(buffer, "data", 4) == 0) {
                found_data = true;
                data_size = chunk_size;
                break; // Found data, ready to read
            } else {
                file.seekg(chunk_size, std::ios::cur);
            }
        }

        if (!found_fmt || !found_data || channels == 0) return false;
        if (bits_per_sample != 16) {
             std::cerr << "Only 16-bit WAV supported." << std::endl;
             return false;
        }

        // Read data
        std::vector<int16_t> int_data(data_size / 2);
        file.read((char*)int_data.data(), data_size);

        // Convert to float mono
        std::vector<float> audio_data;
        audio_data.reserve(int_data.size() / channels);

        for (size_t i = 0; i < int_data.size(); i += channels) {
            float sum = 0.0f;
            for (int c = 0; c < channels; c++) {
                sum += int_data[i + c] / 32768.0f;
            }
            audio_data.push_back(sum / channels);
        }

        reference_audio_ = audio_data;
        ref_sample_rate_ = sample_rate;
        return true;
    }

    std::vector<float> reference_audio_;
    uint32_t ref_sample_rate_ = 44100;

    void analyze_reference() {
        if (reference_audio_.empty()) return;

        // Calculate RMS
        ref_rms_ = calculate_rms(reference_audio_);

        // Calculate Band Energies
        setup_crossover_proto(ref_sample_rate_);

        float e_low = 0.0f, e_mid = 0.0f, e_high = 0.0f;
        measure_bands(reference_audio_, e_low, e_mid, e_high);

        ref_energy_[0] = e_low;
        ref_energy_[1] = e_mid;
        ref_energy_[2] = e_high;
    }

    float calculate_rms(const std::vector<float>& buffer) {
        if (buffer.empty()) return 0.0f;
        float sum_sq = 0.0f;
        for (float s : buffer) sum_sq += s * s;
        return std::sqrt(sum_sq / buffer.size());
    }

    void setup_crossover_proto(uint32_t sr) {
        setup_biquad_coeffs(lp_proto_, 0, 300.0f, sr, 0.707f);
        setup_biquad_coeffs(hp_proto_, 1, 4000.0f, sr, 0.707f);
    }

    void setup_filters() {
        setup_crossover_proto(sample_rate_);

        // Reset state
        for (int i=0; i<2; i++) {
            eq_low_[i].reset();
            eq_mid_[i].reset();
            eq_high_[i].reset();
        }
        // Initial Coeffs
        update_filters_for_gains(1.0f, 1.0f, 1.0f, 0);
        update_filters_for_gains(1.0f, 1.0f, 1.0f, 1);
    }

    void update_filters_for_gains(float g_low, float g_mid, float g_high, int ch) {
        update_gain(eq_low_[ch], 3, 100.0f, sample_rate_, 0.5f, g_low);
        update_gain(eq_mid_[ch], 2, 1000.0f, sample_rate_, 1.0f, g_mid);
        update_gain(eq_high_[ch], 4, 10000.0f, sample_rate_, 0.5f, g_high);
    }

    void setup_biquad_coeffs(Biquad& bq, int type, float freq, float sr, float Q, float dbGain = 0.0f) {
        float w0 = 2.0f * 3.14159265f * freq / sr;
        float alpha = sin(w0) / (2.0f * Q);
        float A = pow(10.0f, dbGain / 40.0f);
        float cos_w0 = cos(w0);
        float a0 = 1.0f;

        switch (type) {
            case 0: // LowPass
                bq.b0 = (1 - cos_w0) / 2; bq.b1 = 1 - cos_w0; bq.b2 = (1 - cos_w0) / 2;
                a0 = 1 + alpha; bq.a1 = -2 * cos_w0; bq.a2 = 1 - alpha;
                break;
            case 1: // HighPass
                bq.b0 = (1 + cos_w0) / 2; bq.b1 = -(1 + cos_w0); bq.b2 = (1 + cos_w0) / 2;
                a0 = 1 + alpha; bq.a1 = -2 * cos_w0; bq.a2 = 1 - alpha;
                break;
            case 2: // Peaking
                bq.b0 = 1 + alpha * A; bq.b1 = -2 * cos_w0; bq.b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; bq.a1 = -2 * cos_w0; bq.a2 = 1 - alpha / A;
                break;
            case 3: // LowShelf
                bq.b0 = A*((A+1)-(A-1)*cos_w0+2*sqrt(A)*alpha);
                bq.b1 = 2*A*((A-1)-(A+1)*cos_w0);
                bq.b2 = A*((A+1)-(A-1)*cos_w0-2*sqrt(A)*alpha);
                a0 = (A+1)+(A-1)*cos_w0+2*sqrt(A)*alpha;
                bq.a1 = -2*((A-1)+(A+1)*cos_w0);
                bq.a2 = (A+1)+(A-1)*cos_w0-2*sqrt(A)*alpha;
                break;
            case 4: // HighShelf
                bq.b0 = A*((A+1)+(A-1)*cos_w0+2*sqrt(A)*alpha);
                bq.b1 = -2*A*((A-1)+(A+1)*cos_w0);
                bq.b2 = A*((A+1)+(A-1)*cos_w0-2*sqrt(A)*alpha);
                a0 = (A+1)-(A-1)*cos_w0+2*sqrt(A)*alpha;
                bq.a1 = 2*((A-1)-(A+1)*cos_w0);
                bq.a2 = (A+1)-(A-1)*cos_w0-2*sqrt(A)*alpha;
                break;
        }

        bq.b0 /= a0; bq.b1 /= a0; bq.b2 /= a0;
        bq.a1 /= a0; bq.a2 /= a0;
    }

    void update_gain(Biquad& bq, int type, float freq, float sr, float Q, float gain_linear) {
        float dbGain = 20.0f * log10(gain_linear);
        dbGain = std::clamp(dbGain, -12.0f, 12.0f);
        setup_biquad_coeffs(bq, type, freq, sr, Q, dbGain);
    }

    void measure_bands(const std::vector<float>& buffer, float& low, float& mid, float& high) {
        float sum_low = 0, sum_mid = 0, sum_high = 0;

        // Use temp filters with SAME coeffs as proto but ZERO state
        Biquad lp = lp_proto_; lp.reset();
        Biquad hp = hp_proto_; hp.reset();

        for (float s : buffer) {
            float l = lp.process(s);
            float h = hp.process(s);
            float m = s - l - h;

            sum_low += l * l;
            sum_mid += m * m;
            sum_high += h * h;
        }

        low = std::sqrt(sum_low / buffer.size());
        mid = std::sqrt(sum_mid / buffer.size());
        high = std::sqrt(sum_high / buffer.size());
    }
};

// Single serialized control writer; one audio reader. All payload fields are
// atomic, so a failed snapshot read is race-free. Sequential consistency
// makes the version check coherent across fields. The reader never retries or
// waits: it simply keeps its last complete configuration during publication.
class MatcheringControl {
public:
    struct Snapshot {
        MatcheringEngine::ReferenceProfile reference;
        MatcheringEngine::Parameters parameters;
        uint32_t generation = 0;
    };
    void publish(const Snapshot& s) noexcept {
        version_.fetch_add(1);
        generation_.store(s.generation);
        ready_.store(s.reference.ready);
        rms_.store(s.reference.rms);
        for (size_t i=0;i<3;++i) energy_[i].store(s.reference.energy[i]);
        smoothing_.store(s.parameters.smoothing);
        amount_.store(s.parameters.eq_amount);
        mix_.store(s.parameters.mix);
        version_.fetch_add(1);
    }
    bool try_read(Snapshot& output) const noexcept {
        const auto version = version_.load();
        if (version & 1u) return false;
        Snapshot s;
        s.generation = generation_.load();
        s.reference.ready = ready_.load();
        s.reference.rms = rms_.load();
        for (size_t i=0;i<3;++i) s.reference.energy[i] = energy_[i].load();
        s.parameters = {smoothing_.load(),amount_.load(),mix_.load()};
        if (version_.load() != version) return false;
        output = s;
        return true;
    }
private:
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free, "Matchering requires lock-free control atomics");
    std::atomic<uint32_t> version_{0}, generation_{0};
    std::atomic<bool> ready_{false};
    std::atomic<float> rms_{0.1f}, energy_[3]{{0.1f},{0.1f},{0.1f}};
    std::atomic<float> smoothing_{0.5f}, amount_{1.f}, mix_{1.f};
};

} // namespace stts
