#pragma once
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace stts {

struct ImageFrame {
    uint8_t* data;
    uint32_t width;
    uint32_t height;
    uint32_t channels; // Usually 4 (BGRA in OBS)
    uint32_t row_bytes;
};

// Generic structure for Vision landmarks/detects
struct VisionResult {
    bool has_detection = false;
    std::vector<std::pair<float, float>> landmarks; // Normalized [0, 1]
    float score = 0.0f;
};

class VisionEngine {
public:
    virtual ~VisionEngine() = default;

    // Load model
    virtual bool initialize(const std::string& config) = 0;

    // Process a video frame
    virtual VisionResult process(const ImageFrame& frame) = 0;

    virtual std::string get_name() const = 0;
    virtual bool is_ready() const = 0;
};

} // namespace stts
