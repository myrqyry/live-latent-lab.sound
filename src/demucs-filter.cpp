#include <obs-module.h>
#include <vector>
#include <thread>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <array>

#include "engines/demucs_engine.hpp"
#include "engines/demucs_cpp_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/ring-buffer.hpp"
#include "framework/stem_output_guard.hpp"
#include "framework/obs_helpers.h"

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct demucs_filter_data {
    obs_source_t *context;

    // Engine
    std::unique_ptr<stts::InferenceEngine> engine;
    std::string model_path;
    std::string model_type; // "onnx" or "cpp"
    int output_mode; // 0=vocals, 1=instrumental, 2=drums, 3=bass, 4=other, 5=multi-channel, 6=custom, 7=dmca-safe

    // Custom mix toggles (for mode 6)
    bool include_drums;
    bool include_bass;
    bool include_other;
    bool include_vocals;

    // Performance
    int num_threads;
    int chunk_ms;

    // Audio Buffering (lock-free input ring buffer)
    std::unique_ptr<stts::RingBuffer> input_ring;

    // Single-stem output modes (0-4, 6, 7) use a lock-free output ring buffer
    std::unique_ptr<stts::RingBuffer> output_ring;

    // Multi-channel output buffers (mode 5) — still deque-based since each stem needs separate tracking
    std::array<std::deque<float>, 4> stem_buffers;

    // Custom mix / DMCA safe buffer (modes 6, 7) — separate from single-stem output
    std::deque<float> custom_mix_buffer;
    std::mutex mix_mutex; // protects custom_mix_buffer and stem_buffers

    // Processing Thread
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    uint32_t sample_rate;

    // Safety
    std::mutex engine_mutex; // Protects engine pointer during reloads

    // Buffer capacity
    static constexpr size_t RING_CAPACITY_SECONDS = 30;
};

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static void demucs_process_thread(demucs_filter_data *data) {
    while (!data->stop_thread.load()) {
        std::vector<float> current_chunk;

        size_t chunk_size = (data->sample_rate * data->chunk_ms) / 1000;
        chunk_size = std::max(chunk_size, (size_t)(data->sample_rate * 2));

        size_t available = data->input_ring->available();
        if (available >= chunk_size) {
            current_chunk.resize(chunk_size);
            data->input_ring->read(current_chunk.data(), chunk_size);
        }

        if (current_chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (data->engine && data->engine->is_ready()) {
                if (data->output_mode == 5 || data->output_mode == 6 || data->output_mode == 7) {
                    std::vector<std::vector<float>> stems;
                    try {
                        stems = data->engine->process_all_stems(current_chunk, data->sample_rate);
                    } catch (const std::exception& e) {
                        blog(LOG_WARNING, "[Demucs] Processing error: %s", e.what());
                    }
                    // Any missing, short or non-finite stems are a failed
                    // inference batch, never a valid separated audio output.
                    if (!stts::stem_batch_is_valid(stems, 4, current_chunk.size()))
                        continue;

                    std::lock_guard<std::mutex> lock(data->mix_mutex);
                    if (data->output_mode == 5) {
                        for (size_t stem = 0; stem < 4 && stem < stems.size(); stem++) {
                            for (float s : stems[stem]) {
                                data->stem_buffers[stem].push_back(s);
                            }
                        }
                    } else if (data->output_mode == 7) {
                        if (stems.size() > 3) {
                            for (float s : stems[3]) {
                                data->custom_mix_buffer.push_back(s);
                            }
                        }
                    } else {
                        size_t num_samples = stems[0].size();
                        for (size_t i = 0; i < num_samples; i++) {
                            float mixed = 0.0f;
                            int count = 0;
                            if (data->include_drums && stems.size() > 0) { mixed += stems[0][i]; count++; }
                            if (data->include_bass && stems.size() > 1) { mixed += stems[1][i]; count++; }
                            if (data->include_other && stems.size() > 2) { mixed += stems[2][i]; count++; }
                            if (data->include_vocals && stems.size() > 3) { mixed += stems[3][i]; count++; }
                            if (count > 1) mixed = mixed / std::sqrt(static_cast<float>(count));
                            data->custom_mix_buffer.push_back(mixed);
                        }
                    }
                } else {
                    std::vector<float> processed;
                    try {
                        processed = data->engine->process(current_chunk, data->sample_rate);
                    } catch (const std::exception& e) {
                        blog(LOG_WARNING, "[Demucs] Processing error: %s", e.what());
                        processed = std::move(current_chunk);
                    }
                    data->output_ring->write(processed.data(), processed.size());
                }
            } else {
                // Single-stem filter modes keep their existing dry bypass.
                // Multi-stem, custom mix and DMCA-safe modes MUST stay silent
                // when no model is available: input music is not separation.
                if (data->output_mode >= 0 && data->output_mode <= 4)
                    data->output_ring->write(current_chunk.data(), current_chunk.size());
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* OBS UI Properties                                                         */
/* ------------------------------------------------------------------------- */

static bool mode_changed_callback(obs_properties_t *props, obs_property_t *list,
                                   obs_data_t *settings) {
    UNUSED_PARAMETER(list);

    int mode = (int)obs_data_get_int(settings, "output_mode");
    bool is_custom = (mode == 6);
    bool is_multichannel = (mode == 5);

    obs_property_set_visible(obs_properties_get(props, "include_drums"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "include_bass"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "include_other"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "include_vocals"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "custom_info"), is_custom);
    obs_property_set_visible(obs_properties_get(props, "multichannel_info"), is_multichannel);

    return true;
}

static obs_properties_t *demucs_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "model_path", "Demucs Model (.onnx)",
                           OBS_PATH_FILE, "*.onnx", nullptr);

    obs_property_t *type = obs_properties_add_list(props, "model_type", "Model Format",
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(type, "Standard ONNX (Waveform)", "onnx");
    obs_property_list_add_string(type, "Advanced (Spectrogram/HTDemucs)", "cpp");

    obs_property_t *mode = obs_properties_add_list(props, "output_mode", "Output Mode",
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, "Vocals Only", 0);
    obs_property_list_add_int(mode, "Instrumental (No Vocals)", 1);
    obs_property_list_add_int(mode, "DMCA Safe (Voice Only)", 7);
    obs_property_list_add_int(mode, "Drums Only", 2);
    obs_property_list_add_int(mode, "Bass Only", 3);
    obs_property_list_add_int(mode, "Other Only", 4);
    obs_property_list_add_int(mode, "Custom Mix...", 6);
    obs_property_list_add_int(mode, "All Stems (Multi-Channel)", 5);

    obs_property_set_modified_callback(mode, mode_changed_callback);

    obs_properties_add_bool(props, "include_drums", "Include Drums");
    obs_properties_add_bool(props, "include_bass", "Include Bass");
    obs_properties_add_bool(props, "include_other", "Include Other");
    obs_properties_add_bool(props, "include_vocals", "Include Vocals");
    obs_properties_add_text(props, "custom_info", "Select which stems to combine.", OBS_TEXT_INFO);
    obs_properties_add_text(props, "multichannel_info", "Ch 1-2: Drums, Ch 3-4: Bass, Ch 5-6: Other, Ch 7-8: Vocals", OBS_TEXT_INFO);

    obs_properties_add_int_slider(props, "chunk_ms", "Processing Chunk (ms)", 2000, 10000, 500);
    obs_properties_add_int(props, "num_threads", "Inference Threads (CPU)", 1, 16, 1);

    obs_property_t *backend = obs_properties_add_list(props, "backend", "Compute Backend",
                                                       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(backend, "Auto (Best Available)", "auto");
    obs_property_list_add_string(backend, "CUDA (NVIDIA GPU)", "cuda");
    obs_property_list_add_string(backend, "DirectML (Windows GPU)", "directml");
    obs_property_list_add_string(backend, "CPU Only", "cpu");

    return props;
}

static void demucs_defaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, "output_mode", 0);
    obs_data_set_default_int(settings, "chunk_ms", 5000);
    obs_data_set_default_int(settings, "num_threads", 4);
    obs_data_set_default_string(settings, "backend", "auto");
    obs_data_set_default_bool(settings, "include_drums", true);
    obs_data_set_default_bool(settings, "include_bass", true);
    obs_data_set_default_bool(settings, "include_other", true);
    obs_data_set_default_bool(settings, "include_vocals", true);
}

static void demucs_update(void *data_ptr, obs_data_t *settings) {
    demucs_filter_data *data = (demucs_filter_data *)data_ptr;

    std::string new_path = safe_obs_data_get_string(settings, "model_path");
    std::string new_type = safe_obs_data_get_string(settings, "model_type");
    int new_mode = (int)obs_data_get_int(settings, "output_mode");
    data->output_mode = new_mode;
    data->chunk_ms = (int)obs_data_get_int(settings, "chunk_ms");
    data->num_threads = (int)obs_data_get_int(settings, "num_threads");
    data->include_drums = obs_data_get_bool(settings, "include_drums");
    data->include_bass = obs_data_get_bool(settings, "include_bass");
    data->include_other = obs_data_get_bool(settings, "include_other");
    data->include_vocals = obs_data_get_bool(settings, "include_vocals");

    bool reload = (new_path != data->model_path || new_type != data->model_type);

    if (reload && !new_path.empty()) {
        data->model_path = new_path;
        data->model_type = new_type;

        std::unique_ptr<stts::InferenceEngine> new_engine;
        bool init_success = false;

        if (data->model_type == "cpp") {
            new_engine = std::make_unique<stts::DemucsCppEngine>();
        } else {
            new_engine = std::make_unique<stts::DemucsEngine>();
        }

        std::stringstream ss;
        ss << data->model_path << "|";
        switch (new_mode) {
            case 0: ss << "vocals"; break;
            case 1: ss << "instrumental"; break;
            case 2: ss << "drums"; break;
            case 3: ss << "bass"; break;
            case 4: ss << "other"; break;
            case 5: case 6: case 7: ss << "all"; break;
        }
        ss << "|" << data->num_threads;
        std::string backend = safe_obs_data_get_string(settings, "backend");
        ss << "|" << (backend.empty() ? "auto" : backend);

        try {
            if (new_engine->initialize(ss.str())) {
                init_success = true;
                blog(LOG_INFO, "[Demucs Filter] Model loaded: %s (type: %s, mode: %d, backend: %s)",
                     data->model_path.c_str(), data->model_type.c_str(), new_mode, backend.c_str());
            } else {
                blog(LOG_ERROR, "[Demucs Filter] Failed to initialize model: %s", data->model_path.c_str());
            }
        } catch (const std::exception& e) {
            blog(LOG_ERROR, "[Demucs Filter] Exception during init: %s", e.what());
        }

        if (init_success) {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            data->engine = std::move(new_engine);
        }
    } else if (data->engine) {
        std::lock_guard<std::mutex> e_lock(data->engine_mutex);
        if (data->model_type == "onnx") {
            // Use dynamic_cast for type-safe downcast
            auto* std_engine = dynamic_cast<stts::DemucsEngine*>(data->engine.get());
            if (std_engine) {
                switch (new_mode) {
                    case 0: std_engine->set_output_stem(stts::DemucsEngine::Stem::VOCALS); break;
                    case 1: std_engine->set_output_instrumental(true); break;
                    case 2: std_engine->set_output_stem(stts::DemucsEngine::Stem::DRUMS); break;
                    case 3: std_engine->set_output_stem(stts::DemucsEngine::Stem::BASS); break;
                    case 4: std_engine->set_output_stem(stts::DemucsEngine::Stem::OTHER); break;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static const char *demucs_get_name(void *) {
    return "LLL.sound Stem Separator";
}

static void *demucs_create(obs_data_t *settings, obs_source_t *context) {
    demucs_filter_data *data = new demucs_filter_data();
    data->context = context;
    data->stop_thread.store(false, std::memory_order_relaxed);
    data->sample_rate = audio_output_get_sample_rate(obs_get_audio());
    data->chunk_ms = 5000;
    data->num_threads = 4;
    data->output_mode = 0;
    data->include_drums = true;
    data->include_bass = true;
    data->include_other = true;
    data->include_vocals = true;

    size_t ring_cap = data->sample_rate * demucs_filter_data::RING_CAPACITY_SECONDS;
    data->input_ring = std::make_unique<stts::RingBuffer>(ring_cap);
    data->output_ring = std::make_unique<stts::RingBuffer>(ring_cap);

    demucs_update(data, settings);

    data->worker_thread = std::thread(demucs_process_thread, data);

    return data;
}

static void demucs_destroy(void *data_ptr) {
    demucs_filter_data *data = (demucs_filter_data *)data_ptr;
    data->stop_thread.store(true);
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    delete data;
}

static struct obs_audio_data *demucs_filter_audio(void *data_ptr, struct obs_audio_data *audio) {
    demucs_filter_data *data = (demucs_filter_data *)data_ptr;

    if (!audio || !audio->data[0]) {
        return audio;
    }

    // Capture input (lock-free)
    data->input_ring->write((const float *)audio->data[0], audio->frames);

    if (data->output_mode == 5) {
        // Multi-channel: each stem to its channel pair
        std::lock_guard<std::mutex> lock(data->mix_mutex);
        for (size_t i = 0; i < audio->frames; i++) {
            for (int stem = 0; stem < 4; stem++) {
                float sample = 0.0f;
                if (!data->stem_buffers[stem].empty()) {
                    sample = data->stem_buffers[stem].front();
                    data->stem_buffers[stem].pop_front();
                }
                int left_ch = stem * 2;
                if (left_ch < MAX_AUDIO_CHANNELS && audio->data[left_ch]) {
                    ((float *)audio->data[left_ch])[i] = sample;
                }
                int right_ch = stem * 2 + 1;
                if (right_ch < MAX_AUDIO_CHANNELS && audio->data[right_ch]) {
                    ((float *)audio->data[right_ch])[i] = sample;
                }
            }
        }
    } else if (data->output_mode == 6 || data->output_mode == 7) {
        // Custom mix / DMCA Safe
        float *out_data = (float *)audio->data[0];
        {
            std::lock_guard<std::mutex> lock(data->mix_mutex);
            for (size_t i = 0; i < audio->frames; i++) {
                if (!data->custom_mix_buffer.empty()) {
                    out_data[i] = data->custom_mix_buffer.front();
                    data->custom_mix_buffer.pop_front();
                } else {
                    out_data[i] = 0.0f;
                }
            }
        }
        for (size_t c = 1; c < MAX_AUDIO_CHANNELS; c++) {
            if (audio->data[c]) {
                memcpy(audio->data[c], audio->data[0], audio->frames * sizeof(float));
            }
        }
    } else {
        // Single-stem mode (lock-free)
        float *out_data = (float *)audio->data[0];
        size_t read = data->output_ring->read(out_data, audio->frames);
        if (read < audio->frames) {
            memset(out_data + read, 0, (audio->frames - read) * sizeof(float));
        }
        for (size_t c = 1; c < MAX_AUDIO_CHANNELS; c++) {
            if (audio->data[c]) {
                memcpy(audio->data[c], audio->data[0], audio->frames * sizeof(float));
            }
        }
    }

    return audio;
}

/* ------------------------------------------------------------------------- */
/* Plugin Registration                                                       */
/* ------------------------------------------------------------------------- */

struct obs_source_info demucs_filter = {
    .id = "demucs_audio_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = demucs_get_name,
    .create = demucs_create,
    .destroy = demucs_destroy,
    .get_defaults = demucs_defaults,
    .get_properties = demucs_properties,
    .update = demucs_update,
    .filter_audio = demucs_filter_audio,
};
