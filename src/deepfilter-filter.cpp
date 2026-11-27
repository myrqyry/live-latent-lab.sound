/**
 * @file deepfilter-filter.cpp
 * @brief OBS Audio Filter for AI Noise Suppression (DeepFilterNet)
 */

#include <obs-module.h>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <cmath>

#include <util/platform.h>
#include "framework/deepfilter_transport.hpp"
#include "framework/rt_wake.hpp"
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
    std::unique_ptr<stts::DeepFilterTransport> transport;

    // Threading
    stts::RtWake wake;
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};

    std::mutex engine_mutex;
    std::mutex control_mutex; // settings writers only
    uint32_t sample_rate;

    // Config
    float attenuation; // Not really used for DFNet (it outputs cleaned audio), but maybe mix?
    bool bypass;
    std::atomic<bool> passthrough_on_underrun{true};
};

/* ------------------------------------------------------------------------- */
/* Worker Thread                                                             */
/* ------------------------------------------------------------------------- */

static constexpr size_t CHUNK_SIZE = 480;

static void df_process_thread(df_filter_data *data) {
    // Standard DeepFilterNet frame size is often 480 samples (10ms at 48k)
    // or 960 (20ms).
    // We'll process in chunks of 480 (approx 10ms at 48k, 10.8ms at 44.1k)
    // Ideally we should query the model or make it configurable.
    // Defaulting to 480 samples.

    std::vector<float> input_chunk(CHUNK_SIZE);
    while (!data->stop_thread) {
        const auto ticket=data->wake.sequence();
        bool processed=false;
        if(!data->bypass && data->active.load(std::memory_order_acquire)){
            std::lock_guard<std::mutex> lock(data->engine_mutex);
            if(data->active.load(std::memory_order_acquire) && data->engine && data->engine->is_ready()){
                const auto generation=data->transport->worker_prepare([&]{
                    data->engine->reset_stream();
                });
                if(data->transport->worker_read(generation,input_chunk.data(),CHUNK_SIZE)){
                    auto output_chunk=data->engine->process(input_chunk,data->sample_rate);
                    if(output_chunk.size()!=CHUNK_SIZE ||
                       !std::all_of(output_chunk.begin(),output_chunk.end(),[](float v){return std::isfinite(v);})){
                        data->transport->worker_fault(generation);
                    }else{
                        data->transport->worker_publish(generation,output_chunk.data(),output_chunk.size());
                    }
                    processed=true;
                }
            }
        }
        if(!processed&&!data->stop_thread.load())
            data->wake.wait(ticket,stts::RtWake::event_driven?100:
                (data->active.load(std::memory_order_acquire)?2:10));
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

    std::lock_guard<std::mutex> control(data->control_mutex);
    std::string new_path = safe_obs_data_get_string(settings, "model_path");
    data->attenuation = (float)obs_data_get_double(settings, "attenuation");
    data->passthrough_on_underrun.store(
        obs_data_get_bool(settings, "passthrough_on_underrun"),
        std::memory_order_relaxed);
    // Attenuation might be implemented as dry/wet mix later.

    if (new_path != data->model_path) {
        data->active.store(false, std::memory_order_release);
        data->transport->invalidate();
        std::unique_ptr<stts::DeepFilterEngine> replacement;
        bool loaded = false;
        if (!new_path.empty()) {
            try {
                replacement=std::make_unique<stts::DeepFilterEngine>();
                loaded=replacement->initialize(new_path) && replacement->is_ready();
            } catch (const std::exception& e) {
                blog(LOG_WARNING,"[Sound/DeepFilter] Model initialization error: %s",e.what());
            }
        }
        {
            std::lock_guard<std::mutex> lock(data->engine_mutex);
            data->model_path = new_path;
            data->engine=loaded?std::move(replacement):nullptr;
            data->active.store(loaded, std::memory_order_release);
        }
        data->wake.notify();
        if (!loaded && !new_path.empty())
            blog(LOG_WARNING, "[Sound/DeepFilter] Model not ready; preserving original audio.");
    }
}

static void *df_create(obs_data_t *settings, obs_source_t *context) {
    std::unique_ptr<df_filter_data> owner;
    try {owner=std::make_unique<df_filter_data>();}catch(...){return nullptr;}
    auto* data=owner.get();
    data->context = context;
    data->stop_thread = false;
    auto* host_audio=obs_get_audio();
    data->sample_rate=host_audio?audio_output_get_sample_rate(host_audio):0;
    if(!data->sample_rate)return nullptr;
    data->bypass = false;
    data->passthrough_on_underrun.store(true);

    // Bounded transport; an overflow invalidates both queues and recurrent state.
    const size_t capacity=size_t(data->sample_rate)*5;
    data->transport=std::make_unique<stts::DeepFilterTransport>(capacity,capacity);

    df_update(data, settings);

    try {data->worker_thread = std::thread(df_process_thread, data);}catch(...){return nullptr;}

    return owner.release();
}

static void df_destroy(void *data_ptr) {
    df_filter_data *data = (df_filter_data *)data_ptr;
    data->stop_thread = true;
    data->wake.notify();
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

    // Unconfigured or failed models cannot mute an otherwise healthy source.
    if(!audio || !audio->data[0] || !audio->frames || data->bypass ||
       !data->active.load(std::memory_order_acquire))return audio;

    const size_t frames=audio->frames;
    if(data->transport->process(reinterpret_cast<float*>(audio->data[0]),frames)){
        // Preserve the existing mono wet routing.
        for(size_t c=1;c<MAX_AUDIO_CHANNELS;++c)
            if(audio->data[c])std::memcpy(audio->data[c],audio->data[0],frames*sizeof(float));
    }else if(!data->passthrough_on_underrun.load(std::memory_order_relaxed)){
        for(size_t c=0;c<MAX_AUDIO_CHANNELS;++c)
            if(audio->data[c])std::memset(audio->data[c],0,frames*sizeof(float));
    }

    if(data->transport->worker_ready(CHUNK_SIZE))data->wake.notify();
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
