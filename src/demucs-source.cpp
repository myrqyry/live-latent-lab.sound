/**
 * @file demucs-source.cpp
 * @brief OBS Audio Source for Demucs source separation with multiple outputs
 * 
 * Creates a source that captures audio from another source and
 * separates it into 4 individual audio stems, each as its own output.
 * 
 * This source creates 4 "child" audio sources:
 * - [SourceName] - Drums
 * - [SourceName] - Bass
 * - [SourceName] - Other
 * - [SourceName] - Vocals
 * 
 * Each can be independently routed, monitored, and recorded.
 */

#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <array>
#include <string>
#include <unordered_map>

#include "engines/demucs_engine.hpp"
#include "framework/ring-buffer.hpp"
#include "framework/stem_output_guard.hpp"

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

static const char* STEM_NAMES[] = {"Drums", "Bass", "Other", "Vocals"};
static const int NUM_STEMS = 4;

/* Forward declaration */
struct demucs_source_data;

/* ------------------------------------------------------------------------- */
/* Global Parent Registry                                                    */
/* ------------------------------------------------------------------------- */

static std::mutex g_parent_registry_mutex;
static std::unordered_map<std::string, demucs_source_data*> g_parent_registry;

static void register_parent(const std::string& name, demucs_source_data* data) {
    std::lock_guard<std::mutex> lock(g_parent_registry_mutex);
    g_parent_registry[name] = data;
}

static void unregister_parent(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_parent_registry_mutex);
    g_parent_registry.erase(name);
}

static demucs_source_data* find_parent(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_parent_registry_mutex);
    auto it = g_parent_registry.find(name);
    return (it != g_parent_registry.end()) ? it->second : nullptr;
}

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct demucs_source_data {
    obs_source_t *context;
    std::string source_name;
    
    // Engine
    std::unique_ptr<stts::DemucsEngine> engine;
    std::string model_path;
    
    // Audio capture
    obs_weak_source_t *audio_source;
    std::string audio_source_name;
    std::vector<float> input_buffer;
    std::mutex input_mutex;
    
    // Stem output buffers (lock-free SPSC ring for each stem)
    std::array<std::unique_ptr<stts::RingBuffer>, NUM_STEMS> stem_buffers;
    
    // Child sources for each stem
    std::array<obs_source_t*, NUM_STEMS> stem_sources;
    
    // Processing thread
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    uint32_t sample_rate;
    int chunk_ms;
    int num_threads;
    
    // Safety
    std::mutex engine_mutex; // Protects engine pointer during reloads
};

/* ------------------------------------------------------------------------- */
/* Stem Source Data                                                          */
/* ------------------------------------------------------------------------- */

struct stem_source_data {
    std::string parent_name;
    int stem_index;
    obs_source_t *context;
};

/* ------------------------------------------------------------------------- */
/* Audio Capture Callback                                                    */
/* ------------------------------------------------------------------------- */

static void demucs_audio_capture(void *param, obs_source_t *source,
                                  const struct audio_data *audio_data, bool muted) {
    UNUSED_PARAMETER(source);
    UNUSED_PARAMETER(muted);
    
    demucs_source_data *data = (demucs_source_data *)param;
    
    if (!audio_data || !audio_data->data[0]) return;
    
    const float *samples = (const float *)audio_data->data[0];
    size_t num_samples = audio_data->frames;
    
    std::lock_guard<std::mutex> lock(data->input_mutex);
    data->input_buffer.insert(data->input_buffer.end(), samples, samples + num_samples);
    
    // Limit buffer size (30 seconds max)
    const size_t max_samples = data->sample_rate * 30;
    if (data->input_buffer.size() > max_samples) {
        data->input_buffer.erase(data->input_buffer.begin(),
                                  data->input_buffer.begin() + (data->input_buffer.size() - max_samples));
    }
}

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static void demucs_process_thread(demucs_source_data *data) {
    while (!data->stop_thread) {
        std::vector<float> current_chunk;
        
        {
            std::lock_guard<std::mutex> lock(data->input_mutex);
            size_t chunk_size = (data->sample_rate * data->chunk_ms) / 1000;
            chunk_size = std::max(chunk_size, (size_t)(data->sample_rate * 2));
            
            if (data->input_buffer.size() >= chunk_size) {
                current_chunk.assign(data->input_buffer.begin(), 
                                    data->input_buffer.begin() + chunk_size);
                data->input_buffer.erase(data->input_buffer.begin(), 
                                        data->input_buffer.begin() + chunk_size);
            }
        }

        if (current_chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Do not claim separated stems if inference is unavailable or fails.
        // Process on this worker thread; never block OBS' audio callback.
        std::vector<std::vector<float>> stems;
        {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (data->engine && data->engine->is_ready()) {
                try {
                    stems = data->engine->process_all_stems(current_chunk, data->sample_rate);
                } catch (const std::exception& e) {
                    blog(LOG_ERROR, "[Demucs Source] Model failed: %s", e.what());
                }
            }
        }
        // Validate the *whole* batch before touching any stem ring. A missing
        // model, partial output or invalid samples result in no new stems.
        stts::publish_stems_if_valid(stems, NUM_STEMS, current_chunk.size(),
            [data](size_t stem, const float* samples, size_t count) {
                data->stem_buffers[stem]->write(samples, count);
            });
    }
}

/* ------------------------------------------------------------------------- */
/* Helper: Enumerate Audio Sources                                           */
/* ------------------------------------------------------------------------- */

static bool enum_audio_sources(void *data, obs_source_t *source) {
    obs_properties_t *props = (obs_properties_t *)data;
    obs_property_t *list = obs_properties_get(props, "audio_source");
    
    uint32_t flags = obs_source_get_output_flags(source);
    if (flags & OBS_SOURCE_AUDIO) {
        const char *name = obs_source_get_name(source);
        if (name) {
            obs_property_list_add_string(list, name, name);
        }
    }
    
    return true;
}

/* ------------------------------------------------------------------------- */
/* OBS UI Properties                                                         */
/* ------------------------------------------------------------------------- */

static obs_properties_t *demucs_source_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();

    // Model path
    obs_properties_add_path(props, "model_path", "Demucs Model (.onnx)", 
                           OBS_PATH_FILE, "*.onnx", nullptr);
    
    // Audio source selector
    obs_property_t *audio_src = obs_properties_add_list(props, "audio_source", 
                                                        "Input Audio Source",
                                                        OBS_COMBO_TYPE_LIST, 
                                                        OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(audio_src, "(Select source)", "");
    
    // Enumerate available audio sources
    obs_enum_sources(enum_audio_sources, props);
    
    // Info text
    obs_properties_add_text(props, "info",
        obs_module_text("SourceInfo"), OBS_TEXT_INFO);
    
    obs_properties_add_int_slider(props, "chunk_ms", "Processing Chunk (ms)", 2000, 10000, 500);
    obs_properties_add_int(props, "num_threads", "Inference Threads", 1, 16, 1);
    
    return props;
}

static void demucs_source_defaults(obs_data_t *settings) {
    obs_data_set_default_int(settings, "chunk_ms", 5000);
    obs_data_set_default_int(settings, "num_threads", 4);
}

static void demucs_source_update(void *data_ptr, obs_data_t *settings) {
    demucs_source_data *data = (demucs_source_data *)data_ptr;
    
    std::string new_model = obs_data_get_string(settings, "model_path");
    std::string new_audio_source = obs_data_get_string(settings, "audio_source");
    data->chunk_ms = (int)obs_data_get_int(settings, "chunk_ms");
    data->num_threads = (int)obs_data_get_int(settings, "num_threads");
    
    // Update audio source capture
    if (new_audio_source != data->audio_source_name) {
        // Remove old capture callback
        if (data->audio_source) {
            obs_source_t *old_source = obs_weak_source_get_source(data->audio_source);
            if (old_source) {
                obs_source_remove_audio_capture_callback(old_source, demucs_audio_capture, data);
                obs_source_release(old_source);
            }
            obs_weak_source_release(data->audio_source);
            data->audio_source = nullptr;
        }
        
        // Add new capture callback
        if (!new_audio_source.empty()) {
            obs_source_t *source = obs_get_source_by_name(new_audio_source.c_str());
            if (source) {
                data->audio_source = obs_source_get_weak_source(source);
                obs_source_add_audio_capture_callback(source, demucs_audio_capture, data);
                obs_source_release(source);
                data->audio_source_name = new_audio_source;
                blog(LOG_INFO, "[Demucs Source] Capturing from: %s", new_audio_source.c_str());
            }
        }
    }
    
    // Reload model if changed
    if (new_model != data->model_path && !new_model.empty()) {
        data->model_path = new_model;
        
        std::lock_guard<std::mutex> e_lock(data->engine_mutex);
        if (!data->engine) {
            data->engine = std::make_unique<stts::DemucsEngine>();
        }
        
        std::stringstream ss;
        ss << data->model_path << "|all|" << data->num_threads;
        data->engine->initialize(ss.str());
        
        blog(LOG_INFO, "[Demucs Source] Model loaded: %s", data->model_path.c_str());
    }
}

/* ------------------------------------------------------------------------- */
/* Stem Source Callbacks                                                     */
/* ------------------------------------------------------------------------- */

static const char *stem_source_get_name(void *type_data) {
    UNUSED_PARAMETER(type_data);
    return "Demucs Stem Output";
}

static void *stem_source_create(obs_data_t *settings, obs_source_t *context) {
    stem_source_data *data = new stem_source_data();
    data->context = context;
    data->stem_index = (int)obs_data_get_int(settings, "stem_index");
    data->parent_name = obs_data_get_string(settings, "parent_name");
    
    blog(LOG_INFO, "[Demucs Stem] Created stem %d (%s) for parent: %s",
         data->stem_index, STEM_NAMES[data->stem_index], data->parent_name.c_str());
    
    return data;
}

static void stem_source_destroy(void *data_ptr) {
    stem_source_data *data = (stem_source_data *)data_ptr;
    if (data) {
        blog(LOG_INFO, "[Demucs Stem] Destroying stem %d", data->stem_index);
        delete data;
    }
}

static bool stem_source_audio_render(void *data_ptr, uint64_t *ts_out,
                                      struct obs_source_audio_mix *audio_output,
                                      uint32_t mixers, size_t channels,
                                      size_t sample_rate) {
    UNUSED_PARAMETER(mixers);
    UNUSED_PARAMETER(channels);
    UNUSED_PARAMETER(sample_rate);
    
    stem_source_data *data = (stem_source_data *)data_ptr;
    if (!data) return false;
    
    // Find parent
    demucs_source_data *parent = find_parent(data->parent_name);
    if (!parent) {
        *ts_out = os_gettime_ns();
        return true; // Silent output
    }
    
    *ts_out = os_gettime_ns();
    size_t frames = AUDIO_OUTPUT_FRAMES;
    
    for (size_t mixer = 0; mixer < MAX_AUDIO_MIXES; mixer++) {
        if (!audio_output->output[mixer].data[0]) continue;
        
        float *output = audio_output->output[mixer].data[0];
        size_t available = parent->stem_buffers[data->stem_index]->available();
        size_t to_read = std::min(available, frames);
        
        for (size_t i = 0; i < to_read; i++) {
            float sample;
            parent->stem_buffers[data->stem_index]->read(&sample, 1);
            output[i] = sample;
        }
        for (size_t i = to_read; i < frames; i++) {
            output[i] = 0.0f;
        }
        
        // Copy to all channels
        for (size_t ch = 1; ch < channels && ch < MAX_AV_PLANES; ch++) {
            if (audio_output->output[mixer].data[ch]) {
                memcpy(audio_output->output[mixer].data[ch], output, frames * sizeof(float));
            }
        }
    }
    
    return true;
}

static obs_properties_t *stem_source_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_text(props, "info",
    obs_module_text("StemOutputInfo"), OBS_TEXT_INFO);
    return props;
}

/* Stem source info - registered in plugin-main */
struct obs_source_info demucs_stem_source = {
    .id = "demucs_stem_output",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = stem_source_get_name,
    .create = stem_source_create,
    .destroy = stem_source_destroy,
    .get_properties = stem_source_properties,
    .audio_render = stem_source_audio_render,
};

/* ------------------------------------------------------------------------- */
/* Main Demucs Source Audio Output (Combined)                                */
/* ------------------------------------------------------------------------- */

static bool demucs_source_audio_render(void *data_ptr, uint64_t *ts_out,
                                       struct obs_source_audio_mix *audio_output,
                                       uint32_t mixers, size_t channels,
                                       size_t sample_rate) {
    UNUSED_PARAMETER(mixers);
    UNUSED_PARAMETER(channels);
    UNUSED_PARAMETER(sample_rate);
    
    demucs_source_data *data = (demucs_source_data *)data_ptr;
    
    *ts_out = os_gettime_ns();
    
    // The main source outputs silence - individual stems have their own sources
    // This prevents double-output of samples
    size_t frames = AUDIO_OUTPUT_FRAMES;
    
    for (size_t mixer = 0; mixer < MAX_AUDIO_MIXES; mixer++) {
        if (!audio_output->output[mixer].data[0]) continue;
        memset(audio_output->output[mixer].data[0], 0, frames * sizeof(float));
    }
    
    return true;
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static const char *demucs_source_get_name(void *) {
    return "Live Latent Sound: Multi-Stem Source";
}

static void *demucs_source_create(obs_data_t *settings, obs_source_t *context) {
    demucs_source_data *data = new demucs_source_data();
    data->context = context;
    data->source_name = obs_source_get_name(context);
    data->stop_thread = false;
    data->sample_rate = audio_output_get_sample_rate(obs_get_audio());
    data->chunk_ms = 5000;
    data->num_threads = 4;
    data->audio_source = nullptr;
    
    // Initialize lock-free stem buffers (5 seconds each)
    size_t ring_cap = data->sample_rate * 5;
    for (int i = 0; i < NUM_STEMS; i++) {
        data->stem_buffers[i] = std::make_unique<stts::RingBuffer>(ring_cap);
    }
    
    // Register this parent for stem sources to find
    register_parent(data->source_name, data);
    
    // Initialize stem sources array
    for (int i = 0; i < NUM_STEMS; i++) {
        data->stem_sources[i] = nullptr;
    }
    
    demucs_source_update(data, settings);
    
    // Create child stem sources
    for (int i = 0; i < NUM_STEMS; i++) {
        char stem_name[256];
        snprintf(stem_name, sizeof(stem_name), "%s - %s", 
                 data->source_name.c_str(), STEM_NAMES[i]);
        
        obs_data_t *stem_settings = obs_data_create();
        obs_data_set_int(stem_settings, "stem_index", i);
        obs_data_set_string(stem_settings, "parent_name", data->source_name.c_str());
        
        data->stem_sources[i] = obs_source_create_private(
            "demucs_stem_output", stem_name, stem_settings);
        
        if (data->stem_sources[i]) {
            // Add to current scene or make visible in mixer
            obs_source_set_audio_mixers(data->stem_sources[i], 0xFF); // Enable all mixers
            obs_set_output_source(i + 1, data->stem_sources[i]); // Associate with output channels
            blog(LOG_INFO, "[Demucs Source] Created stem source: %s", stem_name);
        } else {
            blog(LOG_WARNING, "[Demucs Source] Failed to create stem source: %s", stem_name);
        }
        
        obs_data_release(stem_settings);
    }
    
    // Start worker thread
    data->worker_thread = std::thread(demucs_process_thread, data);
    
    blog(LOG_INFO, "[Demucs Source] Created with %d stem outputs", NUM_STEMS);
    
    return data;
}

static void demucs_source_destroy(void *data_ptr) {
    demucs_source_data *data = (demucs_source_data *)data_ptr;
    
    // Unregister from parent registry
    unregister_parent(data->source_name);
    
    data->stop_thread = true;
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    
    // Destroy child stem sources
    for (int i = 0; i < NUM_STEMS; i++) {
        if (data->stem_sources[i]) {
            obs_source_remove(data->stem_sources[i]);
            obs_source_release(data->stem_sources[i]);
            data->stem_sources[i] = nullptr;
        }
    }
    
    // Clean up audio capture
    if (data->audio_source) {
        obs_source_t *source = obs_weak_source_get_source(data->audio_source);
        if (source) {
            obs_source_remove_audio_capture_callback(source, demucs_audio_capture, data);
            obs_source_release(source);
        }
        obs_weak_source_release(data->audio_source);
    }
    
    delete data;
}

static void demucs_source_activate(void *data_ptr) {
    demucs_source_data *data = (demucs_source_data *)data_ptr;
    
    // Re-establish audio capture if needed
    if (!data->audio_source && !data->audio_source_name.empty()) {
        obs_source_t *source = obs_get_source_by_name(data->audio_source_name.c_str());
        if (source) {
            data->audio_source = obs_source_get_weak_source(source);
            obs_source_add_audio_capture_callback(source, demucs_audio_capture, data);
            obs_source_release(source);
        }
    }
}

static void demucs_source_deactivate(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    // Keep capturing in background for consistency
}

/* ------------------------------------------------------------------------- */
/* Plugin Registration                                                       */
/* ------------------------------------------------------------------------- */

struct obs_source_info demucs_source = {
    .id = "demucs_stem_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = demucs_source_get_name,
    .create = demucs_source_create,
    .destroy = demucs_source_destroy,
    .get_defaults = demucs_source_defaults,
    .get_properties = demucs_source_properties,
    .update = demucs_source_update,
    .activate = demucs_source_activate,
    .deactivate = demucs_source_deactivate,
    .audio_render = demucs_source_audio_render,
};
