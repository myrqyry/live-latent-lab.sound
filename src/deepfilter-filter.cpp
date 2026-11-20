/**
 * @file deepfilter-filter.cpp
 * @brief OBS Audio Filter for AI Noise Suppression (DeepFilterNet)
 */

#include <obs-module.h>
#include <vector>
#include <thread>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>

#include <util/platform.h>
#include "framework/ring-buffer.hpp"
#include "engines/deepfilter_engine.hpp"
#include "framework/obs_helpers.h"

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct df_filter_data {
    obs_source_t *context;

    // Engine
    std::unique_ptr<stts::DeepFilterEngine> engine;
    std::string model_path;
    std::atomic<bool> active{false}; // Published only after a successful model load.

    // Buffering
    // DeepFilterNet often requires fixed chunk sizes (e.g. 480 samples @ 48kHz = 10ms)
    // We use ring buffers to adapt OBS chunk sizes to model chunk sizes.
    std::unique_ptr<stts::RingBuffer> input_ring;
    std::unique_ptr<stts::RingBuffer> output_ring;

    // Threading
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};

    std::mutex engine_mutex;
    uint32_t sample_rate;

    // Config
    float attenuation; // Not really used for DFNet (it outputs cleaned audio), but maybe mix?
    bool bypass;
    std::atomic<bool> passthrough_on_underrun{true};
};

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static void df_process_thread(df_filter_data *data) {
    // Standard DeepFilterNet frame size is often 480 samples (10ms at 48k)
    // or 960 (20ms).
    // We'll process in chunks of 480 (approx 10ms at 48k, 10.8ms at 44.1k)
    // Ideally we should query the model or make it configurable.
    // Defaulting to 480 samples.
    const size_t CHUNK_SIZE = 480;

    while (!data->stop_thread) {
        if (data->bypass || !data->active.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        size_t available = data->input_ring->available();
        if (available < CHUNK_SIZE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        std::vector<float> input_chunk(CHUNK_SIZE);
        data->input_ring->read(input_chunk.data(), CHUNK_SIZE);

        std::vector<float> output_chunk;
        {
            // Protect engine access
            std::lock_guard<std::mutex> lock(data->engine_mutex);
            if (data->engine && data->engine->is_ready()) {
                output_chunk = data->engine->process(input_chunk, data->sample_rate);
            }
        }

        if (output_chunk.empty()) output_chunk = input_chunk; // Fallback

        // Write to output ring
        data->output_ring->write(output_chunk.data(), output_chunk.size());
    }
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static const char *df_get_name(void *) {
    return "LLL.sound AI Noise Suppression";
}

static void df_update(void *data_ptr, obs_data_t *settings) {
    df_filter_data *data = (df_filter_data *)data_ptr;

    std::string new_path = safe_obs_data_get_string(settings, "model_path");
    data->attenuation = (float)obs_data_get_double(settings, "attenuation");
    data->passthrough_on_underrun.store(
        obs_data_get_bool(settings, "passthrough_on_underrun"),
        std::memory_order_relaxed);
    // Attenuation might be implemented as dry/wet mix later.

    if (new_path != data->model_path) {
        data->active.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(data->engine_mutex);
        data->model_path = new_path;
        bool loaded = false;
        if (!new_path.empty()) {
            if (!data->engine)
                data->engine = std::make_unique<stts::DeepFilterEngine>();
            loaded = data->engine->initialize(new_path) && data->engine->is_ready();
        }
        data->active.store(loaded, std::memory_order_release);
        if (!loaded && !new_path.empty())
            blog(LOG_WARNING, "[Sound/DeepFilter] Model not ready; preserving original audio.");
    }
}

static void *df_create(obs_data_t *settings, obs_source_t *context) {
    df_filter_data *data = new df_filter_data();
    data->context = context;
    data->stop_thread = false;
    data->sample_rate = audio_output_get_sample_rate(obs_get_audio());
    data->bypass = false;
    data->passthrough_on_underrun.store(true);

    // Buffer: 5 seconds
    data->input_ring = std::make_unique<stts::RingBuffer>(data->sample_rate * 5);
    data->output_ring = std::make_unique<stts::RingBuffer>(data->sample_rate * 5);

    df_update(data, settings);

    data->worker_thread = std::thread(df_process_thread, data);

    return data;
}

static void df_destroy(void *data_ptr) {
    df_filter_data *data = (df_filter_data *)data_ptr;
    data->stop_thread = true;
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    delete data;
}

static void df_defaults(obs_data_t *settings) {
    obs_data_set_default_bool(settings, "passthrough_on_underrun", true);
}

static obs_properties_t *df_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "model_path", "DeepFilterNet Model (.onnx)",
                           OBS_PATH_FILE, "*.onnx", nullptr);

    obs_properties_add_bool(props, "passthrough_on_underrun", "Passthrough audio until filter is ready");
    obs_property_set_long_description(obs_properties_get(props, "passthrough_on_underrun"),
        "When enabled, passes through unprocessed audio during initial warmup instead of silence.");

    // obs_properties_add_float_slider(props, "attenuation", "Attenuation (dB)", -60.0, 0.0, 0.1);

    return props;
}

static struct obs_audio_data *df_filter_audio(void *data_ptr, struct obs_audio_data *audio) {
    df_filter_data *data = (df_filter_data *)data_ptr;

    // Quick check to bypass if not ready, but safe check requires locking if engine can be nullified.
    // However, locking here blocks audio thread.
    // Ideally use a flag 'is_active' or similar.
    // For now, we will just proceed to push to ring buffer. Ring buffer writes are thread-safe (lock-free usually).
    // The worker thread handles the engine lock.

    // But we check `data->engine` for `is_ready()`?
    // Let's remove this check or make it safe.
    // If the engine is not ready, the worker thread will just sleep.
    // We still need to pass audio through or silence it.

    // Unconfigured or failed models cannot mute an otherwise healthy source.
    if (!audio || !audio->data[0] || data->bypass ||
        !data->active.load(std::memory_order_acquire)) return audio;

    uint32_t frames = audio->frames;

    // 1. Push to Input Ring
    // We assume channel 0 is main mono source (or left).
    // If stereo, we might need two engines or interleave?
    // DeepFilterNet is usually Mono.
    // For now, process Ch 0 and copy to Ch 1.

    if (audio->data[0]) {
        data->input_ring->write((float*)audio->data[0], frames);
    }

    // 2. Read from Output Ring
    // We try to read 'frames' amount. If not enough available (latency), we output silence or wait?
    // In real-time, we can't wait too long.
    // Latency handling: We might need to pre-fill buffer or accept initial delay.

    size_t available = data->output_ring->available();

    if (available >= frames) {
        data->output_ring->read((float*)audio->data[0], frames);

        // Copy to other channels
        for (size_t c = 1; c < MAX_AUDIO_CHANNELS; c++) {
            if (audio->data[c]) {
                memcpy(audio->data[c], audio->data[0], frames * sizeof(float));
            }
        }
    } else {
        // Not enough data yet (initial latency) - user can choose behavior
        if (data->passthrough_on_underrun.load(std::memory_order_relaxed)) {
            // Leave audio unchanged - better UX during warmup
            return audio;
        }
        // Silence mode (original behavior)
        memset(audio->data[0], 0, frames * sizeof(float));
        for (size_t c = 1; c < MAX_AUDIO_CHANNELS; c++) {
            if (audio->data[c]) {
                memset(audio->data[c], 0, frames * sizeof(float));
            }
        }
    }

    return audio;
}

/* ------------------------------------------------------------------------- */
/* Register                                                                  */
/* ------------------------------------------------------------------------- */

extern "C" {
struct obs_source_info deepfilter_filter = {
    .id = "deepfilter_noise_suppression",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = df_get_name,
    .create = df_create,
    .destroy = df_destroy,
    .get_defaults = df_defaults,
    .get_properties = df_properties,
    .update = df_update,
    .filter_audio = df_filter_audio,
};
}
