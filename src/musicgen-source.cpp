/**
 * @file musicgen-source.cpp
 * @brief OBS Audio Source for Music Generation (MusicGen)
 */

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

#include "engines/musicgen_engine.hpp"
#include "framework/ring-buffer.hpp"
#include "framework/obs_helpers.h"

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct musicgen_source_data {
    obs_source_t *context;

    // Engine
    std::unique_ptr<stts::MusicGenEngine> engine;
    std::string model_path; // Directory path

    // Audio Buffering
    std::unique_ptr<stts::RingBuffer> audio_ring;
    uint32_t sample_rate;

    // Worker
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};

    // Request State
    std::string pending_prompt;
    float pending_duration;
    bool request_active;
    std::mutex request_mutex;
    os_event_t *work_signal;
};

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static void musicgen_process_thread(musicgen_source_data *data) {
    while (!data->stop_thread) {
        os_event_wait(data->work_signal);
        if (data->stop_thread) break;

        std::string prompt;
        float duration;
        {
            std::lock_guard<std::mutex> lock(data->request_mutex);
            if (!data->request_active) continue;
            prompt = data->pending_prompt;
            duration = data->pending_duration;
            data->request_active = false;
        }

        if (data->engine && data->engine->is_ready()) {
            // Generate
            std::vector<float> audio = data->engine->generate(prompt, duration);

            if (!audio.empty()) {
                data->audio_ring->write(audio.data(), audio.size());
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static const char *musicgen_get_name(void *) {
    return "LLL.sound AI Music Generator";
}

static void musicgen_update(void *data_ptr, obs_data_t *settings) {
    musicgen_source_data *data = (musicgen_source_data *)data_ptr;

    std::string new_path = safe_obs_data_get_string(settings, "model_dir");
    if (new_path != data->model_path) {
        data->model_path = new_path;
        if (!data->model_path.empty()) {
            if (!data->engine) {
                data->engine = std::make_unique<stts::MusicGenEngine>();
            }
            data->engine->initialize(data->model_path);
        }
    }
}

static void *musicgen_create(obs_data_t *settings, obs_source_t *context) {
    musicgen_source_data *data = new musicgen_source_data();
    data->context = context;
    data->sample_rate = 32000; // MusicGen usually 32k
    data->stop_thread = false;

    // Buffer: 60 seconds
    data->audio_ring = std::make_unique<stts::RingBuffer>(data->sample_rate * 60);

    os_event_init(&data->work_signal, OS_EVENT_TYPE_AUTO);
    data->worker_thread = std::thread(musicgen_process_thread, data);

    musicgen_update(data, settings);

    return data;
}

static void musicgen_destroy(void *data_ptr) {
    musicgen_source_data *data = (musicgen_source_data *)data_ptr;

    data->stop_thread = true;
    os_event_signal(data->work_signal);
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    os_event_destroy(data->work_signal);
    delete data;
}

static obs_properties_t *musicgen_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "model_dir", "Model Directory",
                           OBS_PATH_DIRECTORY, nullptr, nullptr);
    obs_property_set_long_description(obs_properties_get(props, "model_dir"),
                                      "Directory containing MusicGen ONNX models (text_encoder, decoder, encodec).");

    obs_properties_add_text(props, "prompt", obs_module_text("MusicPrompt"), OBS_TEXT_DEFAULT);
    obs_properties_add_float(props, "duration", "Duration (s)", 1.0, 30.0, 1.0);

    obs_properties_add_button(props, "generate_btn", "Generate", [](obs_properties_t *, obs_property_t *, void *data_ptr) {
        musicgen_source_data *data = (musicgen_source_data *)data_ptr;
        obs_data_t *settings = obs_source_get_settings(data->context);

        std::string prompt = safe_obs_data_get_string(settings, "prompt");
        float duration = (float)obs_data_get_double(settings, "duration");

        {
            std::lock_guard<std::mutex> lock(data->request_mutex);
            data->pending_prompt = prompt;
            data->pending_duration = duration;
            data->request_active = true;
        }
        os_event_signal(data->work_signal);

        obs_data_release(settings);
        return false;
    });

    return props;
}

static void musicgen_tick(void *data_ptr, float seconds) {
    UNUSED_PARAMETER(seconds);
    musicgen_source_data *data = (musicgen_source_data *)data_ptr;

    size_t avail = data->audio_ring->available();
    if (avail > 0) {
        size_t chunk_size = 1024;
        float buffer[1024];

        size_t read = data->audio_ring->read(buffer, chunk_size);

        if (read > 0) {
            struct obs_source_audio audio_data = {};
            audio_data.data[0] = (uint8_t*)buffer;
            audio_data.frames = (uint32_t)read;
            audio_data.speakers = SPEAKERS_MONO;
            audio_data.samples_per_sec = data->sample_rate;
            audio_data.timestamp = os_gettime_ns();
            audio_data.format = AUDIO_FORMAT_FLOAT;

            obs_source_output_audio(data->context, &audio_data);
        }
    }
}

struct obs_source_info musicgen_source = {
    .id = "musicgen_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = musicgen_get_name,
    .create = musicgen_create,
    .destroy = musicgen_destroy,
    .get_properties = musicgen_properties,
    .update = musicgen_update,
    .video_tick = musicgen_tick
};
