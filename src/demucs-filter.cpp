#include <obs-module.h>
#include <vector>
#include <thread>
#include <mutex>
#include <memory>
#include <atomic>
#include <array>

#include "engines/demucs_engine.hpp"
#include "engines/demucs_cpp_engine.hpp"
#include "framework/audio_utils.hpp"
#include "framework/demucs_rt.hpp"
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
    // Settings written under engine_mutex and consumed by the worker.
    int output_mode=0;
    unsigned include_mask=15;
    int num_threads=4;
    int chunk_ms=5000;
    std::string backend;
    std::mutex control_mutex; // serialize updates; callback never takes this

    // Audio sees one coherent generation+mode tag. Output blocks are tagged
    // before publication, so switching modes cannot replay a previous mix.
    std::atomic<uint64_t> tag{0};
    std::unique_ptr<stts::DemucsCapture> capture;
    std::unique_ptr<stts::DemucsFilterQueue> output;

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
    std::vector<float> current_chunk, mixed;
    uint64_t capture_epoch=0;
    while (!data->stop_thread.load()) {
        size_t chunk_size;
        uint64_t tag;
        {
            std::lock_guard<std::mutex> lock(data->engine_mutex);
            chunk_size=size_t(data->sample_rate)*data->chunk_ms/1000;
            tag=data->tag.load();
        }
        current_chunk.resize(chunk_size);
        if(!data->capture->read_chunk(current_chunk.data(),chunk_size,capture_epoch)){
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::lock_guard<std::mutex> lock(data->engine_mutex);
        if(tag!=data->tag.load())continue; // settings changed while collecting input
        const int mode=data->output_mode;
        auto publish_mono=[&](const std::vector<float>& samples){
            if(samples.size()!=current_chunk.size() || data->capture->epoch()!=capture_epoch)return;
            const float* planes[]={samples.data()};
            data->output->publish(tag,planes,1,samples.size());
        };
        if(data->engine && data->engine->is_ready()){
            if(mode>=5){
                std::vector<std::vector<float>> stems;
                try{
                    stems=data->engine->process_all_stems(current_chunk,data->sample_rate);
                }catch(const std::exception& e){
                    blog(LOG_WARNING,"[Demucs] Processing error: %s",e.what());
                }
                if(!stts::stem_batch_is_valid(stems,4,current_chunk.size()) ||
                   data->capture->epoch()!=capture_epoch)continue;
                if(mode==5){
                    const float* planes[]={stems[0].data(),stems[1].data(),stems[2].data(),stems[3].data()};
                    data->output->publish(tag,planes,4,current_chunk.size());
                }else if(mode==7){
                    publish_mono(stems[3]);
                }else{
                    mixed.resize(current_chunk.size());
                    unsigned count=0;
                    for(unsigned stem=0;stem<4;++stem)if(data->include_mask&(1u<<stem))++count;
                    const float divisor=count>1?std::sqrt(float(count)):1.f;
                    for(size_t i=0;i<mixed.size();++i){
                        float sample=0;
                        for(unsigned stem=0;stem<4;++stem)
                            if(data->include_mask&(1u<<stem))sample+=stems[stem][i];
                        mixed[i]=sample/divisor;
                    }
                    publish_mono(mixed);
                }
            }else{
                std::vector<float> processed;
                try{
                    processed=data->engine->process(current_chunk,data->sample_rate);
                }catch(const std::exception& e){
                    blog(LOG_WARNING,"[Demucs] Processing error: %s",e.what());
                    processed=current_chunk;
                }
                publish_mono(processed);
            }
        }else if(mode>=0 && mode<=4){
            // Retain the legacy delayed dry fallback only for single-stem modes.
            // No unconfigured model may produce purported custom/multi stems.
            publish_mono(current_chunk);
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

    std::lock_guard<std::mutex> control(data->control_mutex);
    const auto path=safe_obs_data_get_string(settings,"model_path");
    const auto type=safe_obs_data_get_string(settings,"model_type");
    auto backend=safe_obs_data_get_string(settings,"backend");
    if(backend.empty())backend="auto";
    const int mode=std::clamp(int(obs_data_get_int(settings,"output_mode")),0,7);
    const int chunk=std::clamp(int(obs_data_get_int(settings,"chunk_ms")),2000,10000);
    const int threads=std::clamp(int(obs_data_get_int(settings,"num_threads")),1,16);
    unsigned mask=0;
    const char* names[]={"include_drums","include_bass","include_other","include_vocals"};
    for(unsigned i=0;i<4;++i)if(obs_data_get_bool(settings,names[i]))mask|=1u<<i;
    const bool reload=path!=data->model_path || type!=data->model_type ||
                      threads!=data->num_threads || backend!=data->backend;
    if(!reload && mode==data->output_mode && chunk==data->chunk_ms && mask==data->include_mask)return;

    // Prepare a replacement off the callback and outside the worker lock.
    // Continue using the previous published configuration until commit.
    std::unique_ptr<stts::InferenceEngine> replacement;
    if(reload && !path.empty()){
        try{
            if(type=="cpp")replacement=std::make_unique<stts::DemucsCppEngine>();
            else replacement=std::make_unique<stts::DemucsEngine>();
            const char* modes[]={"vocals","instrumental","drums","bass","other","all","all","all"};
            std::stringstream ss;
            ss<<path<<"|"<<modes[mode]<<"|"<<threads<<"|"<<backend;
            if(!replacement->initialize(ss.str()))replacement.reset();
        }catch(const std::exception& e){
            blog(LOG_WARNING,"[Demucs] Model initialization error: %s",e.what());
            replacement.reset();
        }
    }

    std::lock_guard<std::mutex> lock(data->engine_mutex);
    if(reload)data->engine=std::move(replacement);
    if(auto* standard=dynamic_cast<stts::DemucsEngine*>(data->engine.get())){
        switch(mode){
            case 0:standard->set_output_stem(stts::DemucsEngine::Stem::VOCALS);break;
            case 1:standard->set_output_instrumental(true);break;
            case 2:standard->set_output_stem(stts::DemucsEngine::Stem::DRUMS);break;
            case 3:standard->set_output_stem(stts::DemucsEngine::Stem::BASS);break;
            case 4:standard->set_output_stem(stts::DemucsEngine::Stem::OTHER);break;
        }
    }
    data->model_path=path;data->model_type=type;data->backend=backend;
    data->num_threads=threads;data->chunk_ms=chunk;data->output_mode=mode;data->include_mask=mask;
    const auto generation=(data->tag.load()>>3)+1;
    data->tag.store((generation<<3)|uint64_t(mode),std::memory_order_release);
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
    auto* host_audio=obs_get_audio();
    data->sample_rate=host_audio?audio_output_get_sample_rate(host_audio):0;
    if(!data->sample_rate){delete data;return nullptr;}
    const size_t ring_cap=size_t(data->sample_rate)*demucs_filter_data::RING_CAPACITY_SECONDS;
    data->capture=std::make_unique<stts::DemucsCapture>(ring_cap);
    data->output=std::make_unique<stts::DemucsFilterQueue>(size_t(data->sample_rate)*10);

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

    data->capture->push(reinterpret_cast<const float*>(audio->data[0]),audio->frames);
    std::array<float*,MAX_AUDIO_CHANNELS> channels{};
    for(size_t c=0;c<channels.size();++c)
        channels[c]=reinterpret_cast<float*>(audio->data[c]);
    data->output->render(data->tag.load(std::memory_order_acquire),channels.data(),channels.size(),audio->frames);

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
