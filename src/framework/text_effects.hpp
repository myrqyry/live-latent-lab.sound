#pragma once

#include "text_renderer.hpp"
#include <obs-module.h>
#include <cmath>
#include <algorithm>

namespace stts {

class AnimationEngine {
public:
    static void apply_fade(AnimatedWord& word, float progress) {
        word.alpha = std::clamp(progress, 0.0f, 1.0f);
    }

    static void apply_pop(AnimatedWord& word, float progress) {
        // Elastic out-ish pop
        float s = 1.0f;
        if (progress < 1.0f) {
            s = 1.0f + 0.3f * std::sin(progress * M_PI * 2.0f) * (1.0f - progress);
        }
        word.scale = s;
        word.alpha = std::clamp(progress * 2.0f, 0.0f, 1.0f);
    }

    static void apply_slide(AnimatedWord& word, float progress, float offset_y) {
        float eased = 1.0f - std::pow(1.0f - progress, 3.0f); // Cubic out
        // word.pos has been reset to original_pos by renderer
        word.pos.y -= offset_y * (1.0f - eased);
        word.alpha = std::clamp(progress * 1.5f, 0.0f, 1.0f);
    }

    static void apply_bounce(AnimatedWord& word, float progress) {
        float b = 0.0f;
        if (progress < 1.0f) {
            b = std::abs(std::sin(progress * M_PI * 3.0f)) * (1.0f - progress) * 20.0f;
        }
        word.pos.y -= b;
        word.alpha = std::clamp(progress * 2.0f, 0.0f, 1.0f);
    }

    static void apply_tunnel(AnimatedWord& word, float progress, float width, float height) {
        // Endless tunnel effect: scale up and move away from center
        // Center assumption: We don't have exact center, but we can approximate or rely on scale
        // For a tunnel, we want objects to come FROM the distance (scale 0 -> large)

        float p = progress;
        // If endless, progress can go > 1.0.
        // We want it to start small and grow.

        float scale = p;

        // Non-linear scaling for better 3D feel
        // scale = pow(2.0, p) - 1.0?
        // Let's stick to linear for now, but allow it to grow large

        word.scale = scale;

        // Alpha: fade in at start, fade out if too large?
        if (p < 0.2f) {
            word.alpha = p / 0.2f;
        } else {
            word.alpha = 1.0f;
        }

        // Position: Move from a vanishing point.
        float center_x = width / 2.0f;
        float center_y = height / 2.0f;

        float dx = word.original_pos.x - center_x;
        float dy = word.original_pos.y - center_y;

        // Displace based on scale
        // new_pos = center + diff * scale
        word.pos.x = center_x + dx * scale;
        word.pos.y = center_y + dy * scale;
    }

    static float get_typewriter_progress(const AnimatedWord& word, float progress, size_t char_index, size_t total_chars) {
        UNUSED_PARAMETER(word);
        float char_progress = (float)char_index / (float)total_chars;
        if (progress > char_progress) {
            return std::clamp((progress - char_progress) * 10.0f, 0.0f, 1.0f);
        }
        return 0.0f;
    }

    static void apply_wave(AnimatedWord& word, float progress) {
         // Continuous wave motion based on x position and time
         float time = progress * 5.0f; // Speed factor
         float wave_offset = std::sin(time + word.original_pos.x * 0.02f) * 10.0f; // Amplitude
         word.pos.y += wave_offset;
         word.alpha = 1.0f;
    }

    static void apply_shake(AnimatedWord& word, float progress) {
        // Jitter random position
        // Use pseudo-random based on time to avoid seizure-inducing flicker if frame rate is high
        float time_seed = std::floor(progress * 20.0f); // Update 20 times per "progress unit"
        float r1 = std::sin(time_seed * 12.9898f + word.original_pos.x);
        float r2 = std::cos(time_seed * 78.233f + word.original_pos.y);
        
        word.pos.x += r1 * 5.0f;
        word.pos.y += r2 * 5.0f;
        word.alpha = 1.0f;
    }

    static vec4 apply_rainbow(const AnimatedWord& word, float progress, float char_idx_offset) {
        UNUSED_PARAMETER(word);
        // HSV to RGB conversion for rainbow effect
        // Hue cycles with time and position
        float hue = std::fmod(progress * 2.0f + char_idx_offset * 0.1f, 1.0f);
        
        float r, g, b;
        float h = hue * 6.0f;
        float f = h - std::floor(h);
        float q = 1.0f - f;
        float t = 1.0f - (1.0f - f);
        
        switch((int)h) {
            case 0: r = 1; g = t; b = 0; break;
            case 1: r = q; g = 1; b = 0; break;
            case 2: r = 0; g = 1; b = t; break;
            case 3: r = 0; g = q; b = 1; break;
            case 4: r = t; g = 0; b = 1; break;
            default: r = 1; g = 0; b = q; break;
        }
        
        return {r, g, b, 1.0f};
    }
};

} // namespace stts
