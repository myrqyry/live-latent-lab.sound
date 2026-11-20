// Actual DSP engine regression: real ONNX Runtime initialization (no mocked
// backends), deterministic audio samples, verified spectral and bypass behavior.
#include "engines/mic_engine.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

constexpr unsigned SR = 48000;
constexpr size_t N = 8192;
std::vector<float> tone(float hz, float amp = 0.5f) {
    std::vector<float> out(N);
    for (size_t i = 0; i < N; ++i)
        out[i] = amp * std::sin(float(6.283185307179586 * hz * i / SR));
    return out;
}
double rms(const std::vector<float>& values) {
    double sum = 0;
    for (size_t i = 2048; i < values.size(); ++i)
        sum += double(values[i]) * values[i];
    return std::sqrt(sum / (values.size() - 2048));
}
std::vector<float> run_profile(int mode, const std::vector<float>& input) {
    stts::MicEngine engine;
    bool ready = engine.initialize(std::to_string(mode) + "||cpu");
    assert(ready && engine.is_ready());
    return engine.process(input, SR);
}
int main() {
    auto high = tone(9000), mid = tone(700), loud = tone(550, .8f);
    stts::MicEngine bypass;
    assert(bypass.initialize("0||cpu"));
    bypass.set_vibe(.95f);
    assert(bypass.process(high, SR) == high);

    auto condenser = run_profile(2, high);
    auto ribbon = run_profile(3, high);
    auto tape = run_profile(6, loud);
    double hi = rms(high);
    double condenser_gain = rms(condenser) / hi;
    double ribbon_gain = rms(ribbon) / hi;
    double tape_delta = 0;
    for (size_t i = 2048; i < N; ++i)
        tape_delta += std::abs(double(tape[i]) - loud[i]);
    tape_delta /= N - 2048;

    stts::MicEngine left, right;
    assert(left.initialize("4||cpu") && right.initialize("4||cpu"));
    auto left_out = left.process(mid, SR);
    auto right_out = right.process(std::vector<float>(N, 0.f), SR);
    auto untouched_right = rms(right_out);
    auto active_left = rms(left_out);

    stts::MicEngine absent;
    bool missing_ready = absent.initialize("7||cpu");
    auto dry = absent.process(mid, SR);
    std::printf("MIC_DSP condenser_high_gain=%.3f ribbon_high_gain=%.3f tape_avg_delta=%.3f stereo_left=%.3f stereo_right=%.6f missing_model_ready=%d\n",
        condenser_gain, ribbon_gain, tape_delta, active_left, untouched_right, missing_ready);
    bool pass = condenser_gain > 1.08 && ribbon_gain < 0.9 &&
                tape_delta > .05 && active_left > .02 && untouched_right < 1e-7 &&
                !missing_ready && dry == mid;
    std::printf("MIC_DSP_AUDIO_PASS=%d\n", pass);
    return pass ? 0 : 1;
}
