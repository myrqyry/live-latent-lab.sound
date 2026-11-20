/**
 * @file kokoro-source.cpp
 * @brief OBS Audio Source for Kokoro TTS
 * 
 * Generates audio from text using the Kokoro-82M model.
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>

#include "engines/kokoro_engine.hpp"
#include "engines/flash_sr_engine.hpp"
#include "engines/simple_g2p.hpp"
#include "framework/ring-buffer.hpp"
#include "framework/obs_helpers.h"

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct kokoro_source_data {
    obs_source_t *context;
    
    // Engine
    std::unique_ptr<stts::KokoroEngine> engine;
    std::string model_path;

    // G2P
    std::unique_ptr<stts::SimpleG2P> g2p;
    
    // Upscaler
    std::unique_ptr<stts::FlashSREngine> upscaler;
    std::string flash_sr_path;
    std::atomic<bool> enable_upscaling{false};
    
    // Audio Buffering
    std::unique_ptr<stts::RingBuffer> audio_ring;
    std::atomic<uint32_t> sample_rate{24000};
    std::mutex ring_mutex;
    
    // Worker
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    std::function<void()> work_trigger;
    os_event_t *work_signal;

    // Safety
    std::mutex engine_mutex;
    
    // State
    std::string text_to_speak;
    std::mutex text_mutex;
    float speed;
};

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static void kokoro_process_thread(kokoro_source_data *data) {
    while (!data->stop_thread) {
        os_event_wait(data->work_signal);
        if (data->stop_thread) break;
        
        std::string text;
        float spd;
        {
            std::lock_guard<std::mutex> lock(data->text_mutex);
            text = data->text_to_speak;
            data->text_to_speak.clear();
            spd = data->speed;
        }
        
        if (text.empty()) continue;
        
        std::unique_lock<std::mutex> e_lock(data->engine_mutex);
        if (data->engine && data->engine->is_ready()) {
            // 1. Tokenize
            std::vector<int64_t> tokens;
            if (data->g2p) {
                tokens = data->g2p->tokenize(text);
            } else {
                 // Fallback if G2P not initialized? Should not happen if initialized in create.
                 // Naive fallback
                 for (char c : text) tokens.push_back((int64_t)c);
            }
            
            // 2. Generate (24kHz)
            std::vector<float> audio = data->engine->generate(tokens, spd);
            
            // 2b. Upscale (24kHz -> 48kHz)
            if (data->enable_upscaling.load(std::memory_order_relaxed) && data->upscaler && data->upscaler->is_ready()) {
                  // std::vector<float> upscaled = data->upscaler->process(audio, 24000);
                  // If successful, swap.
                  // audio = upscaled;
                  // Note: FlashSR usually outputs at 48k. 
                  // So we must ensure audio_ring expects 48k if enabled.
                 // For now, simpler logic:
                  audio = data->upscaler->process(audio, 24000);
            }
            e_lock.unlock();

            // 3. Queue Audio
            if (!audio.empty()) {
                std::lock_guard<std::mutex> r_lock(data->ring_mutex);
                data->audio_ring->write(audio.data(), audio.size());
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static const char *kokoro_get_name(void *) {
    return "LLL.sound Kokoro TTS";
}

static void kokoro_update(void *data_ptr, obs_data_t *settings) {
    kokoro_source_data *data = (kokoro_source_data *)data_ptr;
    
    std::string new_path = safe_obs_data_get_string(settings, "model_path");
    std::string new_sr_path = safe_obs_data_get_string(settings, "flash_sr_path");
    data->enable_upscaling.store(obs_data_get_bool(settings, "enable_upscaling"), std::memory_order_relaxed);
    data->speed = (float)obs_data_get_double(settings, "speed");
    
    // Initialize Kokoro
    if (new_path != data->model_path) {
        data->model_path = new_path;
        if (!data->model_path.empty()) {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (!data->engine) {
                data->engine = std::make_unique<stts::KokoroEngine>();
            }
            data->engine->initialize(data->model_path);
        }
    }
    
    // Initialize FlashSR
    if (new_sr_path != data->flash_sr_path) {
        data->flash_sr_path = new_sr_path;
        if (!data->flash_sr_path.empty()) {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (!data->upscaler) {
                data->upscaler = std::make_unique<stts::FlashSREngine>();
            }
            data->upscaler->initialize(data->flash_sr_path);
        }
    }
    
    // Update Ring Buffer if sample rate changes (logic omitted for brevity, usually implies flush)
    // FlashSR output = 48k. Kokoro output = 24k.
    // If upscaling is toggled, sample rate changes.
    uint32_t target_rate = data->enable_upscaling.load(std::memory_order_relaxed) ? 48000 : 24000;
    if (data->sample_rate.load(std::memory_order_relaxed) != target_rate) {
        std::lock_guard<std::mutex> r_lock(data->ring_mutex);
        data->sample_rate.store(target_rate, std::memory_order_relaxed);
        // Re-init buffer
        data->audio_ring = std::make_unique<stts::RingBuffer>(data->sample_rate.load(std::memory_order_relaxed) * 60);
    }
}

static void *kokoro_create(obs_data_t *settings, obs_source_t *context) {
    kokoro_source_data *data = new kokoro_source_data();
    data->context = context;
    data->sample_rate.store(24000, std::memory_order_relaxed); // Default
    data->stop_thread = false;
    data->speed = 1.0f;
    data->enable_upscaling.store(false, std::memory_order_relaxed);
    
    // 60 seconds buffer
    data->audio_ring = std::make_unique<stts::RingBuffer>(data->sample_rate.load(std::memory_order_relaxed) * 60); 
    
    // Initialize G2P
    data->g2p = std::make_unique<stts::SimpleG2P>();

    os_event_init(&data->work_signal, OS_EVENT_TYPE_AUTO);
    data->worker_thread = std::thread(kokoro_process_thread, data);
    
    kokoro_update(data, settings);
    
    return data;
}

static void kokoro_destroy(void *data_ptr) {
    kokoro_source_data *data = (kokoro_source_data *)data_ptr;
    
    data->stop_thread = true;
    os_event_signal(data->work_signal);
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    os_event_destroy(data->work_signal);
    
    delete data;
}

static obs_properties_t *kokoro_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();
    
    obs_properties_add_path(props, "model_path", "Kokoro ONNX Model", 
                           OBS_PATH_FILE, "*.onnx", nullptr);
    obs_property_set_long_description(obs_properties_get(props, "model_path"), 
                                      "Path to the Kokoro-82M ONNX model file.");
    
    obs_properties_add_path(props, "flash_sr_path", "FlashSR Upscaler (.onnx)", 
                           OBS_PATH_FILE, "*.onnx", nullptr);
    obs_property_set_long_description(obs_properties_get(props, "flash_sr_path"), 
                                      "Optional: Path to FlashSR model to upsample audio to 48kHz.");
                                      
    obs_properties_add_bool(props, "enable_upscaling", "Enable 48kHz Upscaling");
    
    obs_properties_add_float(props, "speed", "Read Speed", 0.5, 2.0, 0.1);
    obs_property_set_long_description(obs_properties_get(props, "speed"), "Playback speed multiplier (1.0 = normal).");
    
    obs_property_t *p = obs_properties_add_text(props, "preview_text", obs_module_text("PreviewText"), OBS_TEXT_DEFAULT);
    obs_property_set_long_description(p, "Enter text and click Refresh/Update to speak.");
    
    obs_properties_add_button(props, "speak_btn", "Speak", [](obs_properties_t *props, obs_property_t *prop, void *data_ptr) {
        UNUSED_PARAMETER(props);
        UNUSED_PARAMETER(prop);
        kokoro_source_data *data = (kokoro_source_data *)data_ptr;
        obs_data_t *settings = obs_source_get_settings(data->context); // Need to get current settings
        const char* text_ptr = obs_data_get_string(settings, "preview_text");
        
        if (text_ptr && *text_ptr) {
            std::string text = text_ptr;
            std::lock_guard<std::mutex> lock(data->text_mutex);
            data->text_to_speak = text;
            os_event_signal(data->work_signal);
        }
        obs_data_release(settings);
        return false;
    });
    
    return props;
}


// We will use the `video_tick` or a dedicated timer to push audio chunks to OBS
// This is often smoother for sources that generate variable length audio.
static void kokoro_tick(void *data_ptr, float seconds) {
    UNUSED_PARAMETER(seconds);
    kokoro_source_data *data = (kokoro_source_data *)data_ptr;
    
    size_t read = 0;
    float buffer[1024];
    {
        std::lock_guard<std::mutex> r_lock(data->ring_mutex);
        size_t avail = data->audio_ring->available();
        if (avail > 0) {
            // Read chunks
            size_t chunk_size = 1024; // Samples
            read = data->audio_ring->read(buffer, chunk_size);
        }
    }

    if (read > 0) {
        struct obs_source_audio audio_data = {};
        audio_data.data[0] = (uint8_t*)buffer;
        audio_data.frames = (uint32_t)read;
        audio_data.speakers = SPEAKERS_MONO;
        audio_data.samples_per_sec = data->sample_rate.load(std::memory_order_relaxed);
        audio_data.timestamp = os_gettime_ns();
        audio_data.format = AUDIO_FORMAT_FLOAT;

        obs_source_output_audio(data->context, &audio_data);
    }
}

struct obs_source_info kokoro_source = {
    .id = "kokoro_tts_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = kokoro_get_name,
    .create = kokoro_create,
    .destroy = kokoro_destroy,
    .get_properties = kokoro_properties,
    .update = kokoro_update,
    .video_tick = kokoro_tick // Use tick to push audio
};
