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
#include "framework/demucs_rt.hpp"
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
    std::unique_ptr<stts::DemucsCapture> capture;
    std::mutex capture_control_mutex; // never acquired by the capture callback
    
    // Stem output buffers (lock-free SPSC ring for each stem)
    std::array<std::shared_ptr<stts::DemucsStemOutput>, NUM_STEMS> stem_buffers;
    
    // Child sources for each stem
    std::array<obs_source_t*, NUM_STEMS> stem_sources;
    
    // Processing thread
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    uint32_t sample_rate;
    std::atomic<int> chunk_ms{5000};
    int num_threads;
    
    // Safety
    std::mutex engine_mutex; // Protects engine pointer during reloads
};

// Bind once while the parent registry guarantees lifetime. The private child
// source then owns only the output queue, not the parent or inference engine.
static std::shared_ptr<stts::DemucsStemOutput> bind_stem(const std::string& name,int index) {
    if(index<0 || index>=NUM_STEMS)return {};
    std::lock_guard<std::mutex> lock(g_parent_registry_mutex);
    auto it=g_parent_registry.find(name);
    if(it==g_parent_registry.end())return {};
    auto output=it->second->stem_buffers[index];
    if(!output || !output->claim())return {};
    return output;
}

/* ------------------------------------------------------------------------- */
/* Stem Source Data                                                          */
/* ------------------------------------------------------------------------- */

struct stem_source_data {
    std::string parent_name;
    int stem_index;
    obs_source_t *context;
    std::shared_ptr<stts::DemucsStemOutput> output;
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
    
    data->capture->push(reinterpret_cast<const float*>(audio_data->data[0]),audio_data->frames);
}

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static void demucs_process_thread(demucs_source_data *data) {
    std::vector<float> current_chunk;
    uint64_t capture_epoch=0;
    while (!data->stop_thread) {
        const size_t chunk_size=size_t(data->sample_rate)*data->chunk_ms.load()/1000;
        current_chunk.resize(chunk_size);
        if(!data->capture->read_chunk(current_chunk.data(),chunk_size,capture_epoch)){
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
        if(data->capture->epoch()==capture_epoch)
            stts::publish_stems_if_valid(stems, NUM_STEMS, current_chunk.size(),
                [data](size_t stem, const float* samples, size_t count) {
                    data->stem_buffers[stem]->publish(samples, count);
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
    data->chunk_ms = std::clamp((int)obs_data_get_int(settings, "chunk_ms"),2000,10000);
    data->num_threads = (int)obs_data_get_int(settings, "num_threads");
    
    // Serialize source replacement with activation (control path only).
    {
        std::lock_guard<std::mutex> capture_lock(data->capture_control_mutex);
        if (new_audio_source != data->audio_source_name || (!new_audio_source.empty() && !data->audio_source)) {
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
        
            data->capture->mark_gap();
            data->audio_source_name = new_audio_source;
            // Add new capture callback
            if (!new_audio_source.empty()) {
                obs_source_t *source = obs_get_source_by_name(new_audio_source.c_str());
                if (source) {
                    data->audio_source = obs_source_get_weak_source(source);
                    obs_source_add_audio_capture_callback(source, demucs_audio_capture, data);
                    obs_source_release(source);
                    blog(LOG_INFO, "[Demucs Source] Capturing from: %s", new_audio_source.c_str());
                }
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
    return "LLL.sound Demucs Stem Output";
}

static void *stem_source_create(obs_data_t *settings, obs_source_t *context) {
    stem_source_data *data = new stem_source_data();
    data->context = context;
    data->stem_index = (int)obs_data_get_int(settings, "stem_index");
    data->parent_name = obs_data_get_string(settings, "parent_name");
    data->output=bind_stem(data->parent_name,data->stem_index);
    blog(LOG_INFO, "[Demucs Stem] Created stem %d for parent: %s (bound=%d)",
         data->stem_index,data->parent_name.c_str(),int(bool(data->output)));
    
    return data;
}

static void stem_source_destroy(void *data_ptr) {
    stem_source_data *data = (stem_source_data *)data_ptr;
    if (data) {
        blog(LOG_INFO, "[Demucs Stem] Destroying stem %d", data->stem_index);
        if(data->output)data->output->release();
        delete data;
    }
}

static bool stem_source_audio_render(void *data_ptr, uint64_t *ts_out,
                                      struct obs_source_audio_mix *audio_output,
                                      uint32_t mixers, size_t channels,
                                      size_t sample_rate) {
    UNUSED_PARAMETER(sample_rate);
    auto* data=static_cast<stem_source_data*>(data_ptr);
    if(!data || !ts_out || !audio_output)return false;
    *ts_out=os_gettime_ns();
    std::array<float,AUDIO_OUTPUT_FRAMES> block{};
    bool requested=false;
    for(size_t mixer=0;mixer<MAX_AUDIO_MIXES;++mixer)
        if(mixers&(1u<<mixer))
            for(size_t ch=0;ch<channels && ch<MAX_AV_PLANES;++ch)
                requested|=audio_output->output[mixer].data[ch]!=nullptr;
    if(!requested)return true;
    if(data->output)data->output->render(block.data(),block.size());
    // One queue read per time block; every enabled mixer receives the same
    // samples. Never consume a separate block for each destination.
    for(size_t mixer=0;mixer<MAX_AUDIO_MIXES;++mixer)
        if(mixers&(1u<<mixer))
            for(size_t ch=0;ch<channels && ch<MAX_AV_PLANES;++ch)
                if(auto* out=audio_output->output[mixer].data[ch])
                    std::memcpy(out,block.data(),block.size()*sizeof(float));

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
    UNUSED_PARAMETER(data_ptr);
    UNUSED_PARAMETER(sample_rate);
    if(!ts_out || !audio_output)return false;
    *ts_out=os_gettime_ns();
    // The parent is silent; child sources produce the individual stems.
    for(size_t mixer=0;mixer<MAX_AUDIO_MIXES;++mixer)
        if(mixers&(1u<<mixer))
            for(size_t ch=0;ch<channels && ch<MAX_AV_PLANES;++ch)
                if(auto* out=audio_output->output[mixer].data[ch])
                    std::fill_n(out,AUDIO_OUTPUT_FRAMES,0.f);

    return true;
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static const char *demucs_source_get_name(void *) {
    return "LLL.sound Multi-Stem Source";
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
    
    data->capture=std::make_unique<stts::DemucsCapture>(size_t(data->sample_rate)*30);
    // Support the maximum 10s inference chunk plus one queued batch.
    const size_t ring_cap=size_t(data->sample_rate)*20;
    for (int i = 0; i < NUM_STEMS; i++) {
        data->stem_buffers[i] = std::make_shared<stts::DemucsStemOutput>(ring_cap);
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
    for(auto& output:data->stem_buffers)output->close();
    // Remove capture before waiting for the worker. OBS waits for dispatch.
    if (data->audio_source) {
        obs_source_t *source = obs_weak_source_get_source(data->audio_source);
        if (source) {
            obs_source_remove_audio_capture_callback(source, demucs_audio_capture, data);
            obs_source_release(source);
        }
        obs_weak_source_release(data->audio_source);
    }
    
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
    
    delete data;
}

static void demucs_source_activate(void *data_ptr) {
    demucs_source_data *data = (demucs_source_data *)data_ptr;
    
    // Re-establish audio capture if needed
    std::lock_guard<std::mutex> capture_lock(data->capture_control_mutex);
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
