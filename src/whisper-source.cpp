/**
 * @file whisper-source.cpp
 * @brief OBS Text Source for Whisper speech-to-text
 * 
 * Displays live transcription with customizable text styling.
 * Captures audio from a specified source and runs Whisper STT.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <deque>

#include "engines/whisper_engine.hpp"
#include "engines/whisper_cpp_engine.hpp"
#include "framework/text_renderer.hpp"
#include "framework/audio_utils.hpp"
#include "framework/ring-buffer.hpp"
#include "framework/obs_helpers.h"
#include <util/platform.h>
#include <util/threading.h>

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct whisper_source_data {
    obs_source_t *context;
    
    // Engines
    std::unique_ptr<stts::InferenceEngine> engine;
    std::string model_format; // "ggml" or "onnx"
    std::string ggml_model_path;
    std::string encoder_path;
    std::string decoder_path;
    std::string language;
    
    // Audio capture (Lock-free ring buffer for real-time safety)
    obs_weak_source_t *audio_source;
    std::string audio_source_name;
    std::unique_ptr<stts::RingBuffer> audio_ring;
    
    // Transcription
    std::string current_text;
    std::deque<std::string> text_history;
    int max_history_lines;
    std::mutex text_mutex;
    
    // Processing thread
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    uint32_t sample_rate;
    int process_interval_ms;
    
    // Text display settings
    uint32_t text_color;
    uint32_t background_color;
    int font_size;
    std::string font_face;
    bool show_background;
    int padding;
    int width;
    int height;
    
    // Rendering
    std::unique_ptr<stts::TextRenderer> renderer;
    std::string font_path;
    std::string animation_style;
    float animation_duration;

    // Safety
    std::mutex engine_mutex;   // Protects engine pointer during reloads
    std::mutex renderer_mutex; // Protects render-facing state and TextRenderer
    std::mutex audio_mutex;    // Serializes audio callback teardown
    os_event_t *work_signal;
};

/* ------------------------------------------------------------------------- */
/* Audio Capture Callback                                                    */
/* ------------------------------------------------------------------------- */

static void whisper_audio_callback(void *param, obs_source_t *source,
                                   const struct audio_data *audio_data, bool muted) {
    UNUSED_PARAMETER(source);
    UNUSED_PARAMETER(muted);
    
    whisper_source_data *data = (whisper_source_data *)param;
    if (!data) return;
    std::lock_guard<std::mutex> audio_lock(data->audio_mutex);
    if (!data->audio_ring || !audio_data || !audio_data->data[0]) return;
    
    // Lock-free write to pre-allocated ring buffer (no allocation, no mutex)
    data->audio_ring->write((const float *)audio_data->data[0], audio_data->frames);

    // Wake the worker once there is enough audio for transcription.
    if (data->audio_ring->available() >= data->sample_rate) {
        os_event_signal(data->work_signal);
    }
}

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static void whisper_process_thread(whisper_source_data *data) {
    while (!data->stop_thread) {
        os_event_wait(data->work_signal);
        if (data->stop_thread) {
            break;
        }

        {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (!data->engine || !data->engine->is_ready()) {
                continue;
            }
        }
        
        // Get audio chunk for transcription (lock-free read from ring buffer)
        size_t available = data->audio_ring->available();
        if (available < data->sample_rate) { // Need at least 1 second of audio
            continue;
        }
        
        std::vector<float> audio_chunk(available);
        data->audio_ring->read(audio_chunk.data(), available);
        
        // Run transcription
        std::string transcription;
        {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (data->engine && data->engine->is_ready()) {
                transcription = data->engine->transcribe(audio_chunk, data->sample_rate);
            }
        }
        
        if (!transcription.empty()) {
            blog(LOG_INFO, "[Whisper Source] Transcribed: '%s'", transcription.c_str());
            std::lock_guard<std::mutex> lock(data->text_mutex);
            
            // Add to history
            data->text_history.push_back(transcription);
            while ((int)data->text_history.size() > data->max_history_lines) {
                data->text_history.pop_front();
            }
            
            {
                std::lock_guard<std::mutex> r_lock(data->renderer_mutex);
                if (data->renderer) {
                    float timestamp = (float)os_gettime_ns() / 1000000000.0f;
                    data->renderer->add_text(transcription, timestamp);
                }
            }
            
            // Build current display text (still needed for some logic or debug)
            data->current_text.clear();
            for (const auto &line : data->text_history) {
                if (!data->current_text.empty()) {
                    data->current_text += "\n";
                }
                data->current_text += line;
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* OBS UI Properties                                                         */
/* ------------------------------------------------------------------------- */

static obs_properties_t *whisper_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();

    // Model format
    obs_property_t *fmt = obs_properties_add_list(props, "model_format", "Model Format",
                                                  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(fmt, "Whisper.cpp (GGML .bin) - FAST", "ggml");
    obs_property_list_add_string(fmt, "ONNX (Encoder/Decoder .onnx)", "onnx");
    obs_property_set_long_description(fmt, "Select the inference backend:\n\n"
        "• Whisper.cpp (GGML): Best for CPU, lower memory, easiest setup.\n"
        "• ONNX: Better for GPU acceleration (CUDA), but requires two model files.");

    // GGML model path
    obs_properties_add_path(props, "ggml_model_path", "GGML Model (.bin)", 
                           OBS_PATH_FILE, "*.bin", nullptr);
    obs_property_set_long_description(obs_properties_get(props, "ggml_model_path"), 
                                      "Path to the GGML model file (e.g. ggml-small.en.bin).");
    
    // ONNX model paths
    obs_property_t *encoder_prop = obs_properties_add_path(props, "encoder_path", "Whisper Encoder (.onnx)", 
                           OBS_PATH_FILE, "*.onnx", nullptr);
    obs_property_set_long_description(encoder_prop, 
        "Path to the Whisper encoder ONNX model.\n"
        "Download from: huggingface.co/openai/whisper-base");
    
    obs_property_t *decoder_prop = obs_properties_add_path(props, "decoder_path", "Whisper Decoder (.onnx)", 
                           OBS_PATH_FILE, "*.onnx", nullptr);
    obs_property_set_long_description(decoder_prop, 
        "Path to the Whisper decoder ONNX model.\n"
        "Required for ONNX inference mode.");

    auto format_changed = [](obs_properties_t *props, obs_property_t *property, obs_data_t *settings) {
        UNUSED_PARAMETER(property);
        std::string fmt = safe_obs_data_get_string(settings, "model_format");
        bool is_ggml = (fmt == "ggml");
        obs_property_set_visible(obs_properties_get(props, "ggml_model_path"), is_ggml);
        obs_property_set_visible(obs_properties_get(props, "encoder_path"), !is_ggml);
        obs_property_set_visible(obs_properties_get(props, "decoder_path"), !is_ggml);
        return true;
    };
    obs_property_set_modified_callback(fmt, format_changed);
    
    obs_property_t *lang = obs_properties_add_list(props, "language", "Language",
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(lang, "Auto", "auto");
    obs_property_list_add_string(lang, "English", "en");
    obs_property_list_add_string(lang, "Spanish", "es");
    obs_property_list_add_string(lang, "French", "fr");
    obs_property_list_add_string(lang, "German", "de");
    obs_property_list_add_string(lang, "Japanese", "ja");
    obs_property_list_add_string(lang, "Korean", "ko");
    obs_property_list_add_string(lang, "Chinese", "zh");
    obs_property_list_add_string(lang, "Russian", "ru");
    obs_property_list_add_string(lang, "Portuguese", "pt");
    obs_property_list_add_string(lang, "Italian", "it");
    obs_property_set_long_description(lang, 
        "Language hint for transcription.\n"
        "• Auto: Whisper detects the language automatically\n"
        "• Specific language: Faster, more accurate for known languages\n\n"
        "Note: Use English models (.en suffix) for best English accuracy.");
    
    // Audio source - enumerate all audio sources
    obs_property_t *audio_src = obs_properties_add_list(props, "audio_source", "Audio Source",
                                                        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(audio_src, "Default (Desktop Audio)", "");
    obs_property_set_long_description(audio_src, 
        "Select which audio source to transcribe:\n"
        "• Default: Uses desktop audio output\n"
        "• Named source: Captures from a specific OBS audio source\n\n"
        "Tip: Use a dedicated microphone source for speech recognition.");
    
    // Add all audio-capable sources
    auto add_audio_source = [](void *param, obs_source_t *source) {
        obs_property_t *list = (obs_property_t *)param;
        uint32_t flags = obs_source_get_output_flags(source);
        
        // Only add sources that output audio
        if (flags & OBS_SOURCE_AUDIO) {
            const char *name = obs_source_get_name(source);
            if (name && *name) {
                obs_property_list_add_string(list, name, name);
            }
        }
        return true;
    };
    obs_enum_sources(add_audio_source, audio_src);
    
    obs_property_t *interval_prop = obs_properties_add_int(props, "process_interval", "Update Interval (ms)", 500, 5000, 100);
    obs_property_set_long_description(interval_prop, 
        "How often to process accumulated audio (milliseconds).\n"
        "• Lower values: More responsive, higher CPU usage\n"
        "• Higher values: Smoother text, lower CPU usage\n\n"
        "Recommended: 1500-2500ms for most use cases.");
    
    obs_property_t *lines_prop = obs_properties_add_int(props, "max_lines", "Max History Lines", 1, 20, 1);
    obs_property_set_long_description(lines_prop, 
        "Number of previous transcription lines to keep visible.\n"
        "Set to 1 for current speech only, higher for subtitle history.");
    
    // Text display settings
    obs_property_t *text_color_prop = obs_properties_add_color(props, "text_color", "Text Color");
    obs_property_set_long_description(text_color_prop, "Color of the transcribed text.");
    
    obs_property_t *bg_color_prop = obs_properties_add_color(props, "bg_color", "Background Color");
    obs_property_set_long_description(bg_color_prop, "Color of the background behind text.");
    
    obs_property_t *show_bg_prop = obs_properties_add_bool(props, "show_bg", "Show Background");
    obs_property_set_long_description(show_bg_prop, 
        "Enable a semi-transparent background behind the text.\n"
        "Improves readability on complex video backgrounds.");
    
    obs_property_t *font_size_prop = obs_properties_add_int(props, "font_size", "Font Size", 12, 128, 1);
    obs_property_set_long_description(font_size_prop, "Size of the transcription text in pixels.");
    
    obs_property_t *padding_prop = obs_properties_add_int(props, "padding", "Padding", 0, 50, 1);
    obs_property_set_long_description(padding_prop, "Space between text and edges of the source boundary.");
    
    obs_property_t *width_prop = obs_properties_add_int(props, "width", "Width", 100, 3840, 10);
    obs_property_set_long_description(width_prop, "Width of the text source in pixels.");
    
    obs_property_t *height_prop = obs_properties_add_int(props, "height", "Height", 50, 2160, 10);
    obs_property_set_long_description(height_prop, "Height of the text source in pixels.");
    
    obs_property_t *threads_prop = obs_properties_add_int(props, "num_threads", "Inference Threads", 1, 16, 1);
    obs_property_set_long_description(threads_prop, 
        "Number of CPU threads for Whisper inference.\n"
        "• 2-4 threads: Good for most systems\n"
        "• Higher values: Faster on many-core CPUs\n\n"
        "Note: Too many threads can increase latency due to overhead.");

    obs_property_t *backend = obs_properties_add_list(props, "backend", "Compute Backend", 
                                                      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(backend, "Auto (Recommended)", "auto");
    obs_property_list_add_string(backend, "NVIDIA CUDA", "cuda");
    obs_property_list_add_string(backend, "NVIDIA TensorRT", "tensorrt");
    obs_property_list_add_string(backend, "DirectML (AMD/Intel/NVIDIA)", "directml");
    obs_property_list_add_string(backend, "CPU", "cpu");
    obs_property_set_long_description(backend, 
        "Hardware accelerator for ONNX models:\n"
        "• Auto: Uses CUDA if available, falls back to CPU\n"
        "• CUDA: NVIDIA GPU acceleration (fastest)\n"
        "• TensorRT: Optimized NVIDIA inference (requires setup)\n"
        "• DirectML: Windows GPU acceleration (AMD/Intel/NVIDIA)\n"
        "• CPU: Works everywhere, slower");

    // Animation settings
    obs_property_t *font_prop = obs_properties_add_font(props, "font_obj", "Font");
    obs_property_set_long_description(font_prop, "Select a system font for text rendering.");

    obs_property_t *anim = obs_properties_add_list(props, "animation_style", "Animation Style", 
                                                    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(anim, "Fade In", "fade");
    obs_property_list_add_string(anim, "Pop / Bounce", "pop");
    obs_property_list_add_string(anim, "Elastic Bounce", "bounce");
    obs_property_list_add_string(anim, "Slide Up", "slide");
    obs_property_list_add_string(anim, "Endless Tunnel", "tunnel");
    obs_property_list_add_string(anim, "Wave", "wave");
    obs_property_list_add_string(anim, "Shake", "shake");
    obs_property_list_add_string(anim, "Rainbow", "rainbow");
    obs_property_set_long_description(anim, 
        "How new text appears and animates:\n"
        "• Fade: Smooth opacity transition\n"
        "• Pop: Bouncy scale entrance\n"
        "• Bounce: Elastic spring effect\n"
        "• Slide: Text slides up from below\n"
        "• Tunnel: Endless 3D depth effect\n"
        "• Wave: Continuous wavy motion\n"
        "• Shake: Vibrating text effect\n"
        "• Rainbow: Cycling color animation");
    
    obs_property_t *anim_dur_prop = obs_properties_add_float(props, "animation_duration", "Animation Duration (s)", 0.1, 2.0, 0.1);
    obs_property_set_long_description(anim_dur_prop, 
        "Duration of the text entrance animation.\n"
        "• 0.3-0.5s: Quick, snappy animations\n"
        "• 0.7-1.0s: Smooth, gentle transitions");
    
    return props;
}

static void whisper_defaults(obs_data_t *settings) {
    obs_data_set_default_string(settings, "model_format", "ggml");
    obs_data_set_default_string(settings, "language", "auto");
    obs_data_set_default_int(settings, "process_interval", 2000);
    obs_data_set_default_int(settings, "max_lines", 3);
    obs_data_set_default_int(settings, "text_color", 0xFFFFFF);
    obs_data_set_default_int(settings, "bg_color", 0x000000);
    obs_data_set_default_bool(settings, "show_bg", true);
    // Default font object
    obs_data_t *font_obj = obs_data_create();
    obs_data_set_string(font_obj, "face", "Arial");
    obs_data_set_int(font_obj, "size", 32);
    obs_data_set_default_obj(settings, "font_obj", font_obj);
    obs_data_release(font_obj);

    obs_data_set_default_int(settings, "padding", 10);
    obs_data_set_default_int(settings, "width", 800);
    obs_data_set_default_int(settings, "height", 150);
    obs_data_set_default_int(settings, "num_threads", 4);
    obs_data_set_default_string(settings, "animation_style", "pop");
    obs_data_set_default_double(settings, "animation_duration", 0.5);
}

static void whisper_update(void *data_ptr, obs_data_t *settings) {
    whisper_source_data *data = (whisper_source_data *)data_ptr;
    
    std::string new_format = safe_obs_data_get_string(settings, "model_format");
    std::string new_ggml = safe_obs_data_get_string(settings, "ggml_model_path");
    std::string new_encoder = safe_obs_data_get_string(settings, "encoder_path");
    std::string new_decoder = safe_obs_data_get_string(settings, "decoder_path");
    data->language = safe_obs_data_get_string(settings, "language");
    data->process_interval_ms = (int)obs_data_get_int(settings, "process_interval");
    {
        std::lock_guard<std::mutex> lock(data->text_mutex);
        data->max_history_lines = (int)obs_data_get_int(settings, "max_lines");
    }
    {
        std::lock_guard<std::mutex> lock(data->audio_mutex);
        data->audio_source_name = obs_data_get_string(settings, "audio_source");
    }
    
    obs_data_t *font_obj = obs_data_get_obj(settings, "font_obj");
    std::string face = safe_obs_data_get_string(font_obj, "face");
    int size = (int)obs_data_get_int(font_obj, "size");
    std::string new_face = !face.empty() ? face : "Arial";
    obs_data_release(font_obj);

    data->animation_style = safe_obs_data_get_string(settings, "animation_style");
    data->animation_duration = (float)obs_data_get_double(settings, "animation_duration");
    
    std::lock_guard<std::mutex> r_lock(data->renderer_mutex);
    if (data->renderer) {
        data->text_color = (uint32_t)obs_data_get_int(settings, "text_color");
        data->background_color = (uint32_t)obs_data_get_int(settings, "bg_color");
        data->show_background = obs_data_get_bool(settings, "show_bg");
        data->padding = (int)obs_data_get_int(settings, "padding");
        data->width = (int)obs_data_get_int(settings, "width");
        data->height = (int)obs_data_get_int(settings, "height");

        if (new_face != data->font_face || size != data->font_size) {
            data->font_face = new_face;
            data->font_size = size;

            std::string font_path_found = "";
            char command[256];
            snprintf(command, sizeof(command), "fc-match -f \"%%{file}\" \"%s\"", new_face.c_str());
            FILE *fp = popen(command, "r");
            if (fp) {
                char path[512];
                if (fgets(path, sizeof(path), fp) != NULL) {
                     // Remove newline
                     size_t len = strlen(path);
                     if (len > 0 && path[len-1] == '\n') path[len-1] = '\0';
                     font_path_found = path;
                }
                pclose(fp);
            }
            
            if (!font_path_found.empty()) {
                blog(LOG_INFO, "[Whisper Source] Loading font '%s' from path: %s", new_face.c_str(), font_path_found.c_str());
                data->renderer->load_font(font_path_found, data->font_size);
            } else {
                 blog(LOG_WARNING, "[Whisper Source] Could not resolve path for font '%s'", new_face.c_str());
            }
            // For now, I'll use a hardcoded fallback or simple guess for common fonts if I can't find it.
            
         }
        
        stts::TextRenderer::AnimationParams params;
        params.style = data->animation_style;
        params.duration = data->animation_duration;
        params.canvas_width = (float)data->width;
        params.canvas_height = (float)data->height;
        data->renderer->set_animation_params(params);
        
        float r = (float)((data->text_color >> 16) & 0xFF) / 255.0f;
        float g = (float)((data->text_color >> 8) & 0xFF) / 255.0f;
        float b = (float)(data->text_color & 0xFF) / 255.0f;
        data->renderer->set_color({r, g, b, 1.0f});
    }

    int num_threads = (int)obs_data_get_int(settings, "num_threads");
    std::string backend = safe_obs_data_get_string(settings, "backend");
    
    // Reload model if format or paths changed
    bool reload = (new_format != data->model_format || 
                   new_ggml != data->ggml_model_path ||
                   new_encoder != data->encoder_path || 
                   new_decoder != data->decoder_path);
    
    if (reload) {
        data->model_format = new_format;
        data->ggml_model_path = new_ggml;
        data->encoder_path = new_encoder;
        data->decoder_path = new_decoder;
        
        std::unique_ptr<stts::InferenceEngine> new_engine = nullptr;
        bool init_success = false;
        
        if (data->model_format == "ggml" && !data->ggml_model_path.empty()) {
            auto cpp_engine = std::make_unique<stts::WhisperCppEngine>();
            std::stringstream ss;
            ss << data->ggml_model_path << "|" << num_threads << "|" << data->language << "|false";
            
            if (cpp_engine->initialize(ss.str())) {
                new_engine = std::move(cpp_engine);
                init_success = true;
                blog(LOG_INFO, "[Whisper Source] Initialized GGML engine: %s", data->ggml_model_path.c_str());
            } else {
                blog(LOG_ERROR, "[Whisper Source] Failed to initialize GGML engine");
            }
            
        } else if (data->model_format == "onnx" && !data->encoder_path.empty()) {
            auto onnx_engine = std::make_unique<stts::WhisperEngine>();
            std::stringstream ss;
            ss << data->encoder_path << "|" << data->decoder_path << "|" 
               << num_threads << "|" << data->language << "|" << (backend.empty() ? "auto" : backend);
               
            // Use try-catch for ONNX initialization safety
            try {
                if (onnx_engine->initialize(ss.str())) {
                    new_engine = std::move(onnx_engine);
                    init_success = true;
                    blog(LOG_INFO, "[Whisper Source] Initialized ONNX engine (backend: %s)", backend.c_str());
                } else {
                    blog(LOG_ERROR, "[Whisper Source] Failed to initialize ONNX engine");
                }
            } catch (const std::exception& e) {
                 blog(LOG_ERROR, "[Whisper Source] ONNX Exception: %s", e.what());
            }
        }
        
        if (init_success && new_engine) {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            data->engine = std::move(new_engine);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

static void whisper_render(void *data_ptr, gs_effect_t *effect) {
    UNUSED_PARAMETER(effect);
    whisper_source_data *data = (whisper_source_data *)data_ptr;

    std::lock_guard<std::mutex> r_lock(data->renderer_mutex);
    if (data->show_background) {
        gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
        struct vec4 bg_color;
        // Convert hex to vec4
        float br = (float)((data->background_color >> 16) & 0xFF) / 255.0f;
        float bg = (float)((data->background_color >> 8) & 0xFF) / 255.0f;
        float bb = (float)(data->background_color & 0xFF) / 255.0f;
        vec4_set(&bg_color, br, bg, bb, 0.8f);
        gs_effect_set_vec4(color_param, &bg_color);
        
        while (gs_effect_loop(solid, "Solid")) {
            gs_draw_sprite(nullptr, 0, data->width, data->height);
        }
    }
    
    if (data->renderer) {
        float time = (float)os_gettime_ns() / 1000000000.0f;
        data->renderer->render(time);
    }
}

static uint32_t whisper_get_width(void *data_ptr) {
    whisper_source_data *data = (whisper_source_data *)data_ptr;
    std::lock_guard<std::mutex> r_lock(data->renderer_mutex);
    return data->width;
}

static uint32_t whisper_get_height(void *data_ptr) {
    whisper_source_data *data = (whisper_source_data *)data_ptr;
    std::lock_guard<std::mutex> r_lock(data->renderer_mutex);
    return data->height;
}

static enum gs_color_space whisper_get_color_space(void *data_ptr, size_t idx,
                                                   const enum gs_color_space *preferred)
{
    UNUSED_PARAMETER(data_ptr);
    UNUSED_PARAMETER(idx);
    if (preferred && *preferred)
        return *preferred;
    return GS_CS_SRGB;
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static const char *whisper_get_name(void *) {
    return "Live Latent Sound: Animated Captions";
}

static void *whisper_create(obs_data_t *settings, obs_source_t *context) {
    whisper_source_data *data = new whisper_source_data();
    data->context = context;
    data->stop_thread = false;
    data->sample_rate = 48000;
    data->process_interval_ms = 2000;
    data->max_history_lines = 3;
    
    // Pre-allocate 30 seconds of audio buffer (lock-free, no runtime allocation)
    size_t ring_capacity = data->sample_rate * 30;
    data->audio_ring = std::make_unique<stts::RingBuffer>(ring_capacity);
    data->audio_source = nullptr;
    
    // Initialize Renderer
    data->renderer = std::make_unique<stts::TextRenderer>();
    os_event_init(&data->work_signal, OS_EVENT_TYPE_AUTO);
    
    // Default display settings
    data->width = 800;
    data->height = 150;
    data->font_size = 32;
    data->padding = 10;
    data->text_color = 0xFFFFFF;
    data->background_color = 0x000000;
    data->show_background = true;
    
    whisper_update(data, settings);
    
    // Start worker thread
    data->worker_thread = std::thread(whisper_process_thread, data);
    
    return data;
}

static void whisper_destroy(void *data_ptr) {
    whisper_source_data *data = (whisper_source_data *)data_ptr;
    
    std::lock_guard<std::mutex> audio_lock(data->audio_mutex);
    // Remove callback before joining so the audio thread stops touching the ring buffer.
    if (data->audio_source) {
        obs_source_t *source = obs_weak_source_get_source(data->audio_source);
        if (source) {
            obs_source_remove_audio_capture_callback(source, whisper_audio_callback, data);
            obs_source_release(source);
        }
        obs_weak_source_release(data->audio_source);
        data->audio_source = nullptr;
    }

    data->stop_thread = true;
    os_event_signal(data->work_signal);
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }

    // Clean up renderer (clear_cache handles graphics context internally)
    {
        std::lock_guard<std::mutex> r_lock(data->renderer_mutex);
        data->renderer.reset();
    }
    os_event_destroy(data->work_signal);
    
    delete data;
}

static void whisper_activate(void *data_ptr) {
    whisper_source_data *data = (whisper_source_data *)data_ptr;
    
    obs_source_t *audio_source = nullptr;
    std::string audio_source_name;
    {
        std::lock_guard<std::mutex> audio_lock(data->audio_mutex);
        audio_source_name = data->audio_source_name;
    }
    
    // Use selected audio source, or default to desktop audio
    if (!audio_source_name.empty()) {
        audio_source = obs_get_source_by_name(audio_source_name.c_str());
        if (audio_source) {
            blog(LOG_INFO, "[Whisper Source] Using audio source: %s", audio_source_name.c_str());
        }
    }
    
    // Fallback to desktop audio (output channel 0)
    if (!audio_source) {
        audio_source = obs_get_output_source(0);
        blog(LOG_INFO, "[Whisper Source] Using default desktop audio");
    }
    
    if (audio_source) {
        std::lock_guard<std::mutex> audio_lock(data->audio_mutex);
        data->audio_source = obs_source_get_weak_source(audio_source);
        obs_source_add_audio_capture_callback(audio_source, whisper_audio_callback, data);
        obs_source_release(audio_source);
    }
}

static void whisper_deactivate(void *data_ptr) {
    whisper_source_data *data = (whisper_source_data *)data_ptr;
    
    std::lock_guard<std::mutex> audio_lock(data->audio_mutex);
    if (data->audio_source) {
        obs_source_t *source = obs_weak_source_get_source(data->audio_source);
        if (source) {
            obs_source_remove_audio_capture_callback(source, whisper_audio_callback, data);
            obs_source_release(source);
        }
        obs_weak_source_release(data->audio_source);
        data->audio_source = nullptr;
    }
}

/* ------------------------------------------------------------------------- */
/* Plugin Registration                                                       */
/* ------------------------------------------------------------------------- */

struct obs_source_info whisper_source = {
    .id = "whisper_text_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
    .get_name = whisper_get_name,
    .create = whisper_create,
    .destroy = whisper_destroy,
    .get_width = whisper_get_width,
    .get_height = whisper_get_height,
    .get_defaults = whisper_defaults,
    .get_properties = whisper_properties,
    .update = whisper_update,
    .activate = whisper_activate,
    .deactivate = whisper_deactivate,
    .video_render = whisper_render,
    .video_get_color_space = whisper_get_color_space,
};
