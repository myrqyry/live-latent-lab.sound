#include <obs-module.h>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <cstring>

// Include our new framework
#include "framework/inference_engine.hpp"
#include "framework/ring-buffer.hpp"
#include "engines/rvc_onnx_engine.hpp"

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct stts_filter_data {
    obs_source_t *context;
    
    // Framework
    std::unique_ptr<stts::InferenceEngine> engine;
    std::string voice_path;
    std::string pitch_path;
    std::string content_path;
    std::string backend;
    std::atomic<bool> ready{false};
    
    // Performance Tuning
    int num_threads{2};
    int voice_sample_rate{0};
    std::atomic<int> chunk_ms{160};
    
    // Audio Buffering (lock-free SPSC ring buffers)
    std::unique_ptr<stts::RingBuffer> input_ring;
    std::unique_ptr<stts::RingBuffer> output_ring;
    
    // Processing Thread
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    uint32_t sample_rate;
    
    // Safety
    std::mutex engine_mutex; // Protects engine pointer during reloads
    
};

/* ------------------------------------------------------------------------- */
/* Worker Thread (The Brain)                                                 */
/* ------------------------------------------------------------------------- */

void process_audio_thread(struct stts_filter_data *data) {
    while (!data->stop_thread.load()) {
        std::vector<float> current_chunk;
        size_t chunk_size = (data->sample_rate *
            static_cast<size_t>(data->chunk_ms.load(std::memory_order_relaxed))) / 1000;
        if (chunk_size == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        
        // Read from lock-free input ring
        size_t available = data->input_ring->available();
        if (available >= chunk_size) {
            current_chunk.resize(chunk_size);
            data->input_ring->read(current_chunk.data(), chunk_size);
        }

        if (current_chunk.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::vector<float> processed;
        try {
            // engine_mutex protects engine pointer only
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (data->engine && data->engine->is_ready()) {
                processed = data->engine->process(current_chunk, data->sample_rate);
                if (!data->engine->is_ready())
                    data->ready.store(false, std::memory_order_release);
            }
        } catch (const std::exception& e) {
            data->ready.store(false, std::memory_order_release);
            blog(LOG_WARNING, "[STTS] Processing error: %s", e.what());
        }

        // Never enqueue late dry audio as if neural conversion succeeded.
        // The callback stays dry until a new explicit, valid reconfiguration.
        if (data->ready.load(std::memory_order_acquire) && !processed.empty())
            data->output_ring->write(processed.data(), processed.size());
    }
}

/* ------------------------------------------------------------------------- */
/* OBS UI Properties                                                         */
/* ------------------------------------------------------------------------- */

static obs_properties_t *stts_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "voice_path", "RVC Model (Character)", OBS_PATH_FILE, "RVC Models (*.onnx *.model);;All Files (*.*)", nullptr);
    obs_properties_add_path(props, "pitch_path", "Pitch Model (RMVPE)", OBS_PATH_FILE, "*.onnx", nullptr);
    obs_properties_add_path(props, "content_path", "Content Model (HuBERT)", OBS_PATH_FILE, "*.onnx", nullptr);
    
    obs_properties_add_int_slider(props, "chunk_ms", "Latency (Chunk Size ms)", 64, 512, 16);
    obs_properties_add_int(props, "num_threads", "Inference Threads", 1, 16, 1);
    obs_property_t* output_rate = obs_properties_add_list(
        props, "voice_sample_rate", "Exported RVC voice model sample rate",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(output_rate, "Choose verified model rate (required)", 0);
    obs_property_list_add_int(output_rate, "32,000 Hz", 32000);
    obs_property_list_add_int(output_rate, "40,000 Hz", 40000);
    obs_property_list_add_int(output_rate, "48,000 Hz", 48000);
    obs_property_set_long_description(output_rate,
        "Do not guess. Match the exported generator's training/output sample rate. "
        "The RVC inference path is experimental and has not passed end-to-end model testing.");
    
    obs_property_t *backend = obs_properties_add_list(props, "backend", "Compute Backend", 
                                                      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(backend, "Auto (Recommended)", "auto");
    obs_property_list_add_string(backend, "NVIDIA CUDA", "cuda");
    obs_property_list_add_string(backend, "NVIDIA TensorRT", "tensorrt");
    obs_property_list_add_string(backend, "DirectML (AMD/Intel/NVIDIA)", "directml");
    obs_property_list_add_string(backend, "CPU", "cpu");
    
    return props;
}

static void stts_update(void *data_ptr, obs_data_t *settings) {
    struct stts_filter_data *data = (struct stts_filter_data *)data_ptr;
    
    std::string v_path = obs_data_get_string(settings, "voice_path");
    std::string p_path = obs_data_get_string(settings, "pitch_path");
    std::string c_path = obs_data_get_string(settings, "content_path");
    
    const int chunk = (int)obs_data_get_int(settings, "chunk_ms");
    data->chunk_ms.store(chunk >= 64 && chunk <= 512 ? chunk : 160, std::memory_order_relaxed);
    int threads = (int)obs_data_get_int(settings, "num_threads");
    if (threads < 1) threads = 2;
    const int output_rate = int(obs_data_get_int(settings, "voice_sample_rate"));
    const char* backend_str = obs_data_get_string(settings, "backend");
    const std::string backend = backend_str && *backend_str ? backend_str : "auto";

    const bool changed = v_path != data->voice_path || p_path != data->pitch_path ||
        c_path != data->content_path || threads != data->num_threads ||
        backend != data->backend || output_rate != data->voice_sample_rate;
    data->num_threads = threads;

    if (changed) {
        data->ready.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> e_lock(data->engine_mutex);
        data->voice_path = v_path;
        data->pitch_path = p_path;
        data->content_path = c_path;
        data->backend = backend;
        data->voice_sample_rate = output_rate;

        // All three models are required for this RVC pipeline. A missing
        // model must be a dry bypass, not a silent ring-buffer underflow.
        bool loaded = false;
        if (!v_path.empty() && !p_path.empty() && !c_path.empty() &&
            (output_rate == 32000 || output_rate == 40000 || output_rate == 48000)) {
            if (!data->engine) data->engine = std::make_unique<stts::RvcOnnxEngine>();
            std::stringstream spec;
            spec << v_path << "|" << p_path << "|" << c_path << "|"
                 << data->num_threads << "|" << backend << "|" << output_rate;
            loaded = data->engine->initialize(spec.str()) && data->engine->is_ready();
        }
        if (loaded && !data->input_ring) {
            // Allocate buffers ONLY after three compatible real models load.
            const size_t capacity = static_cast<size_t>(data->sample_rate) * 2;
            data->input_ring = std::make_unique<stts::RingBuffer>(capacity);
            data->output_ring = std::make_unique<stts::RingBuffer>(capacity);
        }
        data->ready.store(loaded, std::memory_order_release);
        if (loaded && !data->worker_thread.joinable())
            data->worker_thread = std::thread(process_audio_thread, data);
        if (!loaded)
            blog(LOG_INFO, "[STTS] RVC disabled until compatible voice, pitch and content models are ready.");
    }
}

/* ------------------------------------------------------------------------- */
/* OBS Callbacks                                                             */
/* ------------------------------------------------------------------------- */

static void stts_defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings, "chunk_ms", 160);
    obs_data_set_default_int(settings, "num_threads", 2);
    obs_data_set_default_int(settings, "voice_sample_rate", 0);
    obs_data_set_default_string(settings, "backend", "cpu");
}

static const char *stts_get_name(void *unused) {
    UNUSED_PARAMETER(unused);
    return "LLL.sound Voice Transform";
}

static void *stts_create(obs_data_t *settings, obs_source_t *context) {
    struct stts_filter_data *data = new stts_filter_data();
    data->context = context;
    data->stop_thread.store(false, std::memory_order_relaxed);
    audio_t* output = obs_get_audio();
    data->sample_rate = output ? audio_output_get_sample_rate(output) : 48000;

    // Dry bypass starts without model allocations or idle worker threads.
    stts_update(data, settings);

    return data;
}

static void stts_destroy(void *data_ptr) {
    struct stts_filter_data *data = (struct stts_filter_data *)data_ptr;
    data->stop_thread.store(true);
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    delete data;
}

static struct obs_audio_data *stts_filter_audio(void *data_ptr, struct obs_audio_data *audio) {
    struct stts_filter_data *data = (struct stts_filter_data *)data_ptr;

    // Missing/unloaded models must never silence or collapse the source.
    if (!audio || !audio->data[0] ||
        !data->ready.load(std::memory_order_acquire))
        return audio;

    // This model path is mono. Stereo/multichannel input must stay dry until
    // we implement explicit channel downmix/upmix and timeline alignment.
    for (size_t c = 1; c < MAX_AUDIO_CHANNELS; ++c) {
        if (audio->data[c]) return audio;
    }
    if (!data->input_ring || !data->output_ring)
        return audio;
    const float* input = reinterpret_cast<const float*>(audio->data[0]);
    if (data->input_ring->write(input, audio->frames) != audio->frames)
        return audio;
    // Never zero-fill an inference underrun. If a whole output frame isn't
    // ready, leave the original OBS buffer intact. No partial output writes.
    if (data->output_ring->available() >= audio->frames)
        data->output_ring->read(reinterpret_cast<float*>(audio->data[0]), audio->frames);

    return audio;
}

/* ------------------------------------------------------------------------- */
/* Plugin Registration                                                       */
/* ------------------------------------------------------------------------- */

struct obs_source_info stts_rvc_filter = {
    .id = "stts_rvc_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = stts_get_name,
    .create = stts_create,
    .destroy = stts_destroy,
    .get_defaults = stts_defaults,
    .get_properties = stts_properties,
    .update = stts_update,
    .filter_audio = stts_filter_audio,
};
