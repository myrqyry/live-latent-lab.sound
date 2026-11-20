#pragma once
#include <vector>
#include <string>
#include <memory>

namespace stts {

class InferenceEngine {
public:
    virtual ~InferenceEngine() = default;

    // Load models and initialize the engine
    virtual bool initialize(const std::string& model_path) = 0;

    // Process a chunk of audio
    // input: float array of mono audio
    // output: float array of processed audio
    virtual std::vector<float> process(const std::vector<float>& input, uint32_t sample_rate) = 0;

    // Optional: Transcribe audio to text
    virtual std::string transcribe(const std::vector<float>& /*input*/, uint32_t /*sample_rate*/) { return ""; }

    // Optional: Process audio and return multiple stems (e.g. for Demucs)
    virtual std::vector<std::vector<float>> process_all_stems(const std::vector<float>& input, uint32_t sample_rate) {
        return {process(input, sample_rate)};
    }

    virtual std::string get_name() const = 0;
    virtual bool is_ready() const = 0;
};

} // namespace stts
