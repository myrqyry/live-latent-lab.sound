#pragma once
// Only publish a complete, finite stem batch. This is called by the DEMUCS
// worker, never in OBS' real-time audio callback.
#include <cmath>
#include <cstddef>
#include <vector>

namespace stts {
inline bool stem_batch_is_valid(const std::vector<std::vector<float>>& stems,
                                std::size_t expected_stems,
                                std::size_t expected_frames) {
    if (expected_stems == 0 || expected_frames == 0 ||
        stems.size() != expected_stems)
        return false;

    // Check the entire batch before writing anything, so a partial failure
    // cannot appear as valid synchronized stems.
    for (const auto& samples : stems) {
        if (samples.size() != expected_frames)
            return false;
        for (const float sample : samples) {
            if (!std::isfinite(sample))
                return false;
        }
    }
    return true;
}

template <typename Consumer>
bool publish_stems_if_valid(const std::vector<std::vector<float>>& stems,
                            std::size_t expected_stems,
                            std::size_t expected_frames,
                            Consumer&& consume) {
    if (!stem_batch_is_valid(stems, expected_stems, expected_frames))
        return false;
    for (std::size_t i = 0; i < stems.size(); ++i)
        consume(i, stems[i].data(), stems[i].size());
    return true;
}
} // namespace stts
