#pragma once

#include <obs-module.h>
#include <util/threading.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <mutex>

namespace stts {

struct vec2 {
    float x, y;
};

struct vec4 {
    float x, y, z, w; // r, g, b, a
};

struct GlyphInfo {
    gs_texture_t* texture = nullptr;
    int width = 0, height = 0;
    int bearing_x = 0, bearing_y = 0;
    int advance = 0;
    float tex_u1 = 0.0f, tex_v1 = 0.0f, tex_u2 = 1.0f, tex_v2 = 1.0f;

    // RAII Cleanup
    // IMPORTANT: Caller must ensure graphics context is active!
    ~GlyphInfo() {
        if (texture) {
            gs_texture_destroy(texture);
            texture = nullptr;
        }
    }

    // Prevent Copying (Single Ownership)
    GlyphInfo(const GlyphInfo&) = delete;
    GlyphInfo& operator=(const GlyphInfo&) = delete;

    // Allow Moving
    GlyphInfo(GlyphInfo&& other) noexcept 
        : texture(other.texture), width(other.width), height(other.height),
          bearing_x(other.bearing_x), bearing_y(other.bearing_y), 
          advance(other.advance), 
          tex_u1(other.tex_u1), tex_v1(other.tex_v1),
          tex_u2(other.tex_u2), tex_v2(other.tex_v2) {
        other.texture = nullptr;
    }

    GlyphInfo& operator=(GlyphInfo&& other) noexcept {
        if (this != &other) {
            // IMPORTANT: Caller must ensure graphics context is active!
            if (texture) {
                gs_texture_destroy(texture);
            }
            texture = other.texture;
            width = other.width;
            height = other.height;
            bearing_x = other.bearing_x;
            bearing_y = other.bearing_y;
            advance = other.advance;
            tex_u1 = other.tex_u1;
            tex_v1 = other.tex_v1;
            tex_u2 = other.tex_u2;
            tex_v2 = other.tex_v2;
            other.texture = nullptr;
        }
        return *this;
    }

    GlyphInfo() = default;
};

struct AnimatedWord {
    std::string text;
    float start_time;
    float current_time;
    
    // Transform
    vec2 original_pos;
    vec2 pos;
    float scale = 1.0f;
    float rotation = 0.0f;
    
    // Style
    vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    float alpha = 0.0f;
    
    // Layout info
    float width = 0.0f;
    float height = 0.0f;
    
    bool finished = false;
};

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    bool load_font(const std::string& font_path, uint32_t size);
    void render(float timestamp);
    void add_text(const std::string& text, float timestamp);
    
    void set_color(vec4 color) { default_color_ = color; }
    void set_font_size(uint32_t size) { font_size_ = size; }
    
    // Framework mindset: allow setting different "animators"
    struct AnimationParams {
        float duration = 0.5f;
        std::string style = "fade"; // fade, bounce, pop, typewriter, tunnel
        float easing = 1.0f;
        bool endless = false;
        float canvas_width = 800.0f;
        float canvas_height = 600.0f;
    };
    
    void set_animation_params(const AnimationParams& params) { anim_params_ = params; }

private:
    void clear_cache();
    GlyphInfo* get_glyph(uint32_t char_code, bool create_texture = true);
    void update_animations(float timestamp);
    void draw_word(const AnimatedWord& word);

    FT_Library ft_;
    FT_Face face_;
    bool font_loaded_ = false;
    uint32_t font_size_ = 32;
    
    std::map<uint32_t, std::unique_ptr<GlyphInfo>> glyph_cache_;
    std::vector<std::unique_ptr<GlyphInfo>> graveyard_;
    std::vector<AnimatedWord> active_words_;
    
    std::mutex cache_mutex_;
    std::mutex words_mutex_;

    vec4 default_color_ = {1.0f, 1.0f, 1.0f, 1.0f};
    AnimationParams anim_params_;
    
    float last_timestamp_ = 0.0f;
};

} // namespace stts
