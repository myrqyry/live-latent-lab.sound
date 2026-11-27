#pragma once
// Experimental RVC-like chunk stitcher. CPU-only; never called from an OBS
// audio callback. Inspired by SOLA in the user's historic w-okada VC Client.
// Expects *overlapping model output windows*, NOT consecutive PCM packets.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace stts {

struct SolaSettings {
    size_t hop_frames;      // frames consumed/emitted per stream step
    size_t overlap_frames;  // future output provided by previous model window
    size_t search_frames;   // optional alignment offset, 0 = alignment disabled
    float fade_start = 0.2f;
    float fade_end = 1.0f;
};

struct SolaAlignment {
    size_t offset_frames = 0;
    bool alignment_used = false;
};

struct SolaOutput {
    std::vector<float> pcm; // precisely hop_frames samples
    size_t offset_frames = 0;
    bool alignment_used = false;
};

class SolaStitcher {
public:
    explicit SolaStitcher(SolaSettings settings) : settings_(settings) {
        if (!settings_.hop_frames || !settings_.overlap_frames ||
            settings_.overlap_frames > settings_.hop_frames ||
            settings_.search_frames > settings_.hop_frames ||
            !std::isfinite(settings_.fade_start) ||
            !std::isfinite(settings_.fade_end) ||
            settings_.fade_start < 0 || settings_.fade_start >= settings_.fade_end ||
            settings_.fade_end > 1 ||
            settings_.overlap_frames > std::numeric_limits<size_t>::max() -
                                        settings_.search_frames ||
            settings_.hop_frames > std::numeric_limits<size_t>::max() -
                                   (settings_.overlap_frames + settings_.search_frames)) {
            throw std::invalid_argument("invalid SOLA frame or fade configuration");
        }
        previous_tail_.resize(settings_.overlap_frames);
        fade_.resize(settings_.overlap_frames);
        for(size_t i=0;i<fade_.size();++i){
            const double pos=double(i)/double(fade_.size());
            const double start=settings_.fade_start,end=settings_.fade_end;
            const double f=pos<=start?0.0:pos>=end?1.0:(pos-start)/(end-start);
            const double c=std::cos(f*1.57079632679489661923);
            fade_[i]=c*c;
        }
    }

    void reset() {
        std::fill(previous_tail_.begin(), previous_tail_.end(), 0.0f);
        primed_ = false;
    }
    bool primed() const { return primed_; }

    // Each window must have at least hop + overlap + search frames.
    // On a stream's initial window there is no previous tail to match.
    SolaOutput push(const float* samples, size_t frames) {
        SolaOutput result;
        result.pcm.resize(settings_.hop_frames);
        const auto alignment=push_into(samples,frames,result.pcm.data(),result.pcm.size());
        result.offset_frames=alignment.offset_frames;
        result.alignment_used=alignment.alignment_used;
        return result;
    }

    // Non-owning output for a worker that can reuse its hop buffer.
    // Input and output buffers must not overlap.
    SolaAlignment push_into(const float* samples,size_t frames,float* output,size_t output_frames) {
        const auto hop = settings_.hop_frames;
        if(!output || output_frames!=hop)
            throw std::invalid_argument("SOLA output must match hop size");
        const auto overlap = settings_.overlap_frames;
        const auto search = settings_.search_frames;
        if (!samples || frames < hop + overlap + search)
            throw std::invalid_argument("SOLA window is shorter than required lookahead");
        for (size_t i = 0; i < hop + overlap + search; ++i)
            if (!std::isfinite(samples[i]))
                throw std::invalid_argument("SOLA received non-finite audio sample");

        size_t offset = 0;
        bool matched = false;
        if (primed_ && search) {
            double prev_energy = 0.0;
            for (float x : previous_tail_) prev_energy += double(x) * x;
            // Silence provides no reliable phase alignment. Stay at offset 0.
            if (prev_energy > 1e-9 * double(overlap)) {
                double best_score = -std::numeric_limits<double>::infinity();
                for (size_t candidate = 0; candidate <= search; ++candidate) {
                    double dot = 0.0, energy = 0.0;
                    for (size_t i = 0; i < overlap; ++i) {
                        const double sample = samples[candidate + i];
                        dot += sample * previous_tail_[i];
                        energy += sample * sample;
                    }
                    // Mirrors the normalized correlation in the reference:
                    // previous-tail energy is constant over all candidates.
                    // Strict > ensures earliest-offset tie break.
                    if (energy <= 1e-9 * double(overlap)) continue;
                    const double score = dot / std::sqrt(energy + 1e-3);
                    if (score > best_score) {
                        best_score = score;
                        offset = candidate;
                        matched = true;
                    }
                }
            }
        }

        std::copy_n(samples + offset, hop, output);
        if (primed_) {
            for (size_t i = 0; i < overlap; ++i) {
                const double prev_strength=fade_[i];
                const double next_strength=1.0-prev_strength;
                output[i]=float(double(previous_tail_[i])*prev_strength+
                                double(output[i])*next_strength);
            }
        }
        // The next chunk overlaps the *unplayed* output after this hop.
        // It must be raw audio, not a previously faded or emitted region.
        std::copy_n(samples + offset + hop, overlap, previous_tail_.begin());
        primed_ = true;
        return {offset,matched};
    }

private:
    SolaSettings settings_;
    std::vector<float> previous_tail_;
    std::vector<double> fade_;
    bool primed_ = false;
};

} // namespace stts
