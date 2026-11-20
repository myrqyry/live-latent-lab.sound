#include "text_renderer.hpp"
#include "text_effects.hpp"
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <sstream>

namespace stts {

TextRenderer::TextRenderer() {
    if (FT_Init_FreeType(&ft_)) {
        blog(LOG_ERROR, "[TextRenderer] Failed to initialize FreeType");
    }
    face_ = nullptr;
}

TextRenderer::~TextRenderer() {
    clear_cache();
    if (face_) FT_Done_Face(face_);
    FT_Done_FreeType(ft_);
}

bool TextRenderer::load_font(const std::string& font_path, uint32_t size) {
    std::map<uint32_t, std::unique_ptr<GlyphInfo>> old_cache_to_destroy;
    
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        
        if (face_) {
            FT_Done_Face(face_);
            face_ = nullptr;
        }
        
        if (FT_New_Face(ft_, font_path.c_str(), 0, &face_)) {
            blog(LOG_ERROR, "[TextRenderer] Failed to load font: %s", font_path.c_str());
            font_loaded_ = false;
            return false;
        }
        
        FT_Set_Pixel_Sizes(face_, 0, size);
        font_size_ = size;
        font_loaded_ = true;
        
        // Move cache out to destroy after unlocking
        old_cache_to_destroy = std::move(glyph_cache_);
        glyph_cache_.clear();
    }
    
    // Cleanup old textures
    if (!old_cache_to_destroy.empty()) {
        obs_enter_graphics();
        old_cache_to_destroy.clear();
        obs_leave_graphics();
    }
    
    return true;
}

void TextRenderer::clear_cache() {
    // Move cache out of lock, then destroy
    std::map<uint32_t, std::unique_ptr<GlyphInfo>> old_cache;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        old_cache = std::move(glyph_cache_);
        glyph_cache_.clear();
    }
    
    // Destroy glyphs safely inside graphics context
    // (GlyphInfo::~GlyphInfo requires graphics context to be active)
    if (!old_cache.empty()) {
        obs_enter_graphics();
        old_cache.clear();
        obs_leave_graphics();
    }
}

GlyphInfo* TextRenderer::get_glyph(uint32_t char_code, bool create_texture) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = glyph_cache_.find(char_code);
    
    GlyphInfo* info = nullptr;
    if (it != glyph_cache_.end()) {
        info = it->second.get();
    } else {
        if (!font_loaded_ || !face_) return nullptr;
        
        if (FT_Load_Char(face_, char_code, FT_LOAD_RENDER)) {
            return nullptr;
        }
        
        FT_GlyphSlot g = face_->glyph;
        
        auto new_info = std::make_unique<GlyphInfo>();
        new_info->width = g->bitmap.width;
        new_info->height = g->bitmap.rows;
        new_info->bearing_x = g->bitmap_left;
        new_info->bearing_y = g->bitmap_top;
        new_info->advance = (int)(g->advance.x >> 6);
        new_info->tex_u1 = 0.0f;
        new_info->tex_v1 = 0.0f;
        new_info->tex_u2 = 1.0f;
        new_info->tex_v2 = 1.0f;
        
        info = new_info.get();
        glyph_cache_[char_code] = std::move(new_info);
        
        // Enforce cache limit
        const size_t MAX_CACHE_SIZE = 512;
        if (glyph_cache_.size() >= MAX_CACHE_SIZE) {
            auto begin = glyph_cache_.begin();
            auto end = std::next(begin, MAX_CACHE_SIZE / 4); // Remove oldest 25%
            
            // Move to graveyard
            for (auto git = begin; git != end; ++git) {
                if (git->second && git->second->texture) {
                    graveyard_.push_back(std::move(git->second));
                }
            }
            glyph_cache_.erase(begin, end);
        }
    }

    if (create_texture && info && !info->texture) {
         if (FT_Load_Char(face_, char_code, FT_LOAD_RENDER)) return nullptr; 
         FT_GlyphSlot g = face_->glyph;

         obs_enter_graphics();
         info->texture = gs_texture_create(g->bitmap.width, g->bitmap.rows, GS_A8, 1, nullptr, 0);
         
         if (info->texture) {
             uint8_t* ptr;
             uint32_t linesize;
             if (gs_texture_map(info->texture, &ptr, &linesize)) {
                 const uint8_t* src = g->bitmap.buffer;
                 for (unsigned int y = 0; y < g->bitmap.rows; ++y) {
                     memcpy(ptr + y * linesize, src + y * g->bitmap.pitch, g->bitmap.width);
                 }
                 gs_texture_unmap(info->texture);
             }
         }
         obs_leave_graphics();
    }
    
    return info;
}

void TextRenderer::add_text(const std::string& text, float timestamp) {
    if (text.empty()) return;
    
    std::lock_guard<std::mutex> lock(words_mutex_);

    // Simple word splitting
    std::vector<std::string> words;
    std::string current;
    std::stringstream ss(text);
    while (ss >> current) {
        words.push_back(current);
    }

    float x_offset = 20.0f;
    float y_offset = font_size_ + 20.0f;
    float line_height = font_size_ * 1.2f;
    float max_width = 760.0f; 
    
    float word_delay = 0.0f;
    for (const auto& w : words) {
        AnimatedWord word;
        word.text = w;
        word.start_time = timestamp + word_delay;
        word.current_time = timestamp;
        word.color = default_color_;
        word.alpha = 0.0f;
        word.scale = 1.0f;
        
        // Calculate word width
        float w_width = 0;
        for (char c : w) {
            // SAFE: Create texture = false
            GlyphInfo* g = get_glyph((uint32_t)c, false);
            if (g) w_width += g->advance;
        }
        
        if (x_offset + w_width > max_width) {
            x_offset = 20.0f;
            y_offset += line_height;
        }
        
        word.pos = {x_offset, y_offset};
        word.original_pos = word.pos;
        word.width = w_width;
        
        active_words_.push_back(word);
        
        GlyphInfo* space_g = get_glyph(' ', false);
        if (space_g) {
            x_offset += w_width + space_g->advance;
        } else {
            x_offset += w_width + (font_size_ / 4); // Fallback
        }
        word_delay += 0.05f; 
    }
}

void TextRenderer::update_animations(float timestamp) {
    float dt = timestamp - last_timestamp_;
    if (last_timestamp_ <= 0.0f) dt = 0.016f;
    last_timestamp_ = timestamp;
    
    for (auto it = active_words_.begin(); it != active_words_.end(); ) {
        // Reset state to original before applying animations
        it->pos = it->original_pos;

        float elapsed = timestamp - it->start_time;
        float progress = elapsed / anim_params_.duration;
        
        if (!anim_params_.endless) {
            if (progress > 1.0f) progress = 1.0f;
        } else {
             // If endless is enabled but we are using a style that isn't designed for it (like tunnel),
             // we should clamp progress or handle it.
             // "tunnel" is designed for progress > 1.0.
             // "fade", "pop", "slide", "bounce" usually expect 0-1.
             if (anim_params_.style != "tunnel") {
                 if (progress > 1.0f) progress = 1.0f;
             }
        }
        
        // Apply effects based on style
        if (anim_params_.style == "fade") {
            AnimationEngine::apply_fade(*it, progress);
        } else if (anim_params_.style == "pop") {
            AnimationEngine::apply_pop(*it, progress);
        } else if (anim_params_.style == "slide") {
            AnimationEngine::apply_slide(*it, progress, 20.0f);
        } else if (anim_params_.style == "bounce") {
            AnimationEngine::apply_bounce(*it, progress);
        } else if (anim_params_.style == "tunnel") {
            AnimationEngine::apply_tunnel(*it, progress, anim_params_.canvas_width, anim_params_.canvas_height);
        } else if (anim_params_.style == "wave") {
            AnimationEngine::apply_wave(*it, progress);
        } else if (anim_params_.style == "shake") {
            AnimationEngine::apply_shake(*it, progress);
        } else if (anim_params_.style == "rainbow") {
            // Position/Alpha handled normally, color overridden in draw_word
            it->alpha = 1.0f;
        }
        
        float ttl = anim_params_.endless ? 20.0f : 5.0f;

        if (elapsed > ttl) { // TTL for words
            it = active_words_.erase(it);
        } else {
            ++it;
        }
    }
}

void TextRenderer::draw_word(const AnimatedWord& word) {
    if (word.alpha <= 0.0f) return;
    
    float x = word.pos.x;
    float y = word.pos.y;
    
    gs_effect_t* effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t* image_param = gs_effect_get_param_by_name(effect, "image");
    
    while (gs_effect_loop(effect, "Draw")) {
        float cur_x = x;
        size_t char_index = 0;
        size_t total_chars = word.text.length();
        
        for (char c : word.text) {
            // Ensure texture exists now that we are in render thread
            GlyphInfo* g = get_glyph((uint32_t)c, true);
            if (!g) continue;
            
            float char_alpha = word.alpha;
                if (anim_params_.style == "typewriter") {
                    float elapsed = last_timestamp_ - word.start_time;
                    float progress = std::clamp(elapsed / anim_params_.duration, 0.0f, 1.0f);
                    char_alpha = AnimationEngine::get_typewriter_progress(word, progress, char_index, total_chars);
                }
                
                vec4 color = word.color;
                if (anim_params_.style == "rainbow") {
                    float elapsed = last_timestamp_ - word.start_time;
                    float progress = elapsed / anim_params_.duration; // Continuously increasing for loop
                    if (anim_params_.endless) progress = elapsed * 0.5f; // Slower base speed
                    
                    color = AnimationEngine::apply_rainbow(word, progress, (float)char_index);
                }
                
                if (char_alpha > 0.0f) {
                    float gw = g->width * word.scale;
                    float gh = g->height * word.scale;
                    float gy = y - g->bearing_y * word.scale;
                    
                    gs_effect_set_texture(image_param, g->texture);
                    
                    // Set color
                    gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), (const struct ::vec4*)&color);
                
                gs_matrix_push();
                gs_matrix_translate3f(cur_x + g->bearing_x * word.scale, gy, 0.0f);
                gs_draw_sprite(g->texture, 0, (uint32_t)gw, (uint32_t)gh);
                gs_matrix_pop();
            }
            
            cur_x += g->advance * word.scale;
            char_index++;
        }
    }
}

void TextRenderer::render(float timestamp) {
    // 1. Process Graveyard (Destroy old textures safely)
    {
        std::vector<std::unique_ptr<GlyphInfo>> zombies;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            zombies = std::move(graveyard_);
            graveyard_.clear();
        }
        // Zombies destroyed here (GlyphInfo dtor -> gs_texture_destroy), safe because we are in render thread
        zombies.clear();
    }
    
    std::lock_guard<std::mutex> lock(words_mutex_);
    update_animations(timestamp);
    
    for (const auto& word : active_words_) {
        draw_word(word);
    }
}

} // namespace stts
