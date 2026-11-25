// Opt-in native YAMNet. OBS audio callback never runs inference, resampling,
// disk I/O, allocation or mutex operations; original audio passes unchanged.
#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <media-io/audio-io.h>
#include <callback/proc.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "engines/mediapipe_audio_123.hpp"
#include "framework/ring-buffer.hpp"

#include "interop/sound_signal_client.hpp"
namespace {
constexpr uint32_t TARGET_HZ=16000;
constexpr size_t WINDOW=TARGET_HZ,READ_BLOCK=4096,CAPTURE_BLOCK=1024;
struct YamnetWorker {
    YamnetWorker(uint32_t rate,uint32_t channel_count)
        : sample_rate(rate),channels(channel_count),ring(size_t(rate)*3) {}
    const uint32_t sample_rate,channels;
    stts::RingBuffer ring;
    std::string library,model,device="cpu",delegate;
    std::atomic<bool> stopping{false},overloaded{false},started{false};
    std::atomic<int64_t> dropped{0},callback_calls{0};
    std::thread worker;
    std::mutex snapshot_mutex; // Worker and read-only procedure; never callback.
    std::string status="loading",label;
    double score=0;
    std::vector<stts::mp_audio_123::Prediction> categories;
    int64_t windows=0,inference_ms=0;
    std::chrono::steady_clock::time_point last_result{};
};
void fail(YamnetWorker* s,const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(s->snapshot_mutex);
        s->status="error";s->label.clear();s->score=0;
    }
    if(!s->stopping.load())
        blog(LOG_WARNING,"[Sound/YAMNet] %s",message.c_str());
}
void run_worker(YamnetWorker* s) {
    stts::mp_audio_123::Classifier classifier;
    std::string error;
    if(!classifier.open(s->library,s->model,error,1024,s->device)) {
        fail(s,"Cannot open native classifier: "+error);return;
    }
    const resample_info input_info={s->sample_rate,AUDIO_FORMAT_FLOAT_PLANAR,SPEAKERS_MONO};
    const resample_info output_info={TARGET_HZ,AUDIO_FORMAT_FLOAT_PLANAR,SPEAKERS_MONO};
    audio_resampler_t* resampler=audio_resampler_create(&output_info,&input_info);
    if(!resampler){fail(s,"OBS audio resampler unavailable");return;}
    std::array<float,READ_BLOCK> incoming{};
    std::vector<float> window(WINDOW);
    size_t used=0;
    {
        std::lock_guard<std::mutex> lock(s->snapshot_mutex);
        s->status="ready";s->delegate=classifier.delegate();
    }
    s->started.store(true,std::memory_order_release);
    blog(LOG_INFO,"[Sound/YAMNet] SOUND_YAMNET_WORKER_20261113_P641 native "
                  "classifier ready (%u Hz to 16000 Hz), requested=%s delegate=%s",s->sample_rate,s->device.c_str(),s->delegate.c_str());
    while(!s->stopping.load(std::memory_order_acquire) &&
          !s->overloaded.load(std::memory_order_acquire)){
        const size_t count=s->ring.read(incoming.data(),incoming.size());
        if(!count){
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            continue;
        }
        const uint8_t* input[MAX_AUDIO_CHANNELS]{};
        uint8_t* output[MAX_AUDIO_CHANNELS]{};
        input[0]=reinterpret_cast<const uint8_t*>(incoming.data());
        uint32_t frames=0;uint64_t offset=0;
        if(!audio_resampler_resample(resampler,output,&frames,&offset,
                                     input,uint32_t(count)) ||
           (frames && !output[0])){
            fail(s,"OBS resampler failed");break;
        }
        const auto* samples=reinterpret_cast<const float*>(output[0]);
        for(uint32_t i=0;i<frames;++i){
            if(s->stopping.load(std::memory_order_relaxed) ||
               s->overloaded.load(std::memory_order_relaxed))break;
            window[used++]=samples[i];
            if(used!=WINDOW)continue;
            used=0;
            std::vector<stts::mp_audio_123::Prediction> result;
            error.clear();
            const auto begin=std::chrono::steady_clock::now();
            const bool ok=classifier.classify(window.data(),WINDOW,TARGET_HZ,
                                               result,error);
            const auto duration=std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-begin).count();
            if(!ok||result.empty()){
                fail(s,"Native inference failed: "+error);
                s->overloaded.store(true,std::memory_order_release);
                break;
            }
            if(s->stopping.load(std::memory_order_acquire) ||
               s->overloaded.load(std::memory_order_acquire))break;
            std::lock_guard<std::mutex> lock(s->snapshot_mutex);
            s->label=result.front().category;
            s->score=result.front().score;
            s->categories=std::move(result);
            s->inference_ms=duration;
            s->last_result=std::chrono::steady_clock::now();
            ++s->windows;
        }
    }
    audio_resampler_destroy(resampler);
    if(s->overloaded.load()){
        std::lock_guard<std::mutex> lock(s->snapshot_mutex);
        // Preserve an inference error; only an actual queue overflow
        // should be reported as an overflow.
        if(s->dropped.load()>0)s->status="overload";
        s->label.clear();s->score=0;
        blog(LOG_WARNING,"[Sound/YAMNet] Disabled after inference/queue "
                         "failure; recreate filter to retry");
    }
}
void worker_entry(YamnetWorker* s) {
    try{run_worker(s);}
    catch(const std::exception& e){fail(s,std::string("Worker error: ")+e.what());}
    catch(...){fail(s,"Unknown worker error");}
}
void snapshot(void* ptr,calldata_t* output){
    auto* s=static_cast<YamnetWorker*>(ptr);
    if(!s)return;
    std::lock_guard<std::mutex> lock(s->snapshot_mutex);
    // A stopped microphone cannot leave a previous sound classification
    // looking current forever. This clock check runs only on procedure reads.
    const bool expired=s->windows>0 &&
       std::chrono::steady_clock::now()-s->last_result>std::chrono::milliseconds(2500);
    const bool available=s->status=="ready"&&s->windows>0 &&
                          !expired&&!s->overloaded.load();
    calldata_set_string(output,"label",available?s->label.c_str():"");
    calldata_set_float(output,"score",available?s->score:0.);
    calldata_set_int(output,"windows",s->windows);
    calldata_set_int(output,"dropped_samples",s->dropped.load());
    calldata_set_int(output,"inference_ms",s->inference_ms);
    calldata_set_int(output,"callback_calls",s->callback_calls.load());
    calldata_set_string(output,"status",
        expired&&s->status=="ready"?"stale":s->status.c_str());
    calldata_set_bool(output,"available",available);
}
// Control-thread metadata request; actual model outputs establish the
// category names. Retain names after expiry, but never expose stale scores.
void list_categories(void* ptr,calldata_t* call){
    auto* s=static_cast<YamnetWorker*>(ptr);
    void* raw=nullptr;
    if(!s||!calldata_get_ptr(call,"inventory",&raw)||!raw)return;
    auto* inventory=static_cast<obs_data_t*>(raw);
    auto* rows=obs_data_array_create();
    std::vector<stts::mp_audio_123::Prediction> copy;
    {std::lock_guard<std::mutex> lock(s->snapshot_mutex);copy=s->categories;}
    std::sort(copy.begin(),copy.end(),[](const auto& a,const auto& b){return a.index<b.index;});
    for(const auto& c:copy){
        if(c.category.empty()||c.category.size()>256)continue;
        auto* row=obs_data_create();
        obs_data_set_string(row,"label",c.category.c_str());
        obs_data_set_int(row,"index",c.index);
        obs_data_array_push_back(rows,row);obs_data_release(row);
    }
    obs_data_set_int(inventory,"version",1);
    obs_data_set_array(inventory,"categories",rows);obs_data_array_release(rows);
}
void category_score(void* ptr,calldata_t* call){
    auto* s=static_cast<YamnetWorker*>(ptr);
    const char* name=calldata_string(call,"category");
    bool available=false;
    double score=0;
    if(s&&name&&*name){
        std::lock_guard<std::mutex> lock(s->snapshot_mutex);
        if(s->status=="ready"&&s->windows>0&&!s->overloaded.load()&&
           std::chrono::steady_clock::now()-s->last_result<=std::chrono::milliseconds(2500)){
            auto it=std::find_if(s->categories.begin(),s->categories.end(),
                [&](const auto& p){return p.category==name;});
            if(it!=s->categories.end()){available=true;score=it->score;}
        }
    }
    calldata_set_float(call,"score",score);
    calldata_set_bool(call,"available",available);
}
void device_status(void* ptr,calldata_t* d){
    auto* s=static_cast<YamnetWorker*>(ptr);if(!s)return;
    std::lock_guard lock(s->snapshot_mutex);
    calldata_set_string(d,"requested",s->device.c_str());
    calldata_set_string(d,"selected",s->delegate.c_str());
    calldata_set_string(d,"status",s->status.c_str());
}
void defaults(obs_data_t* s){
    obs_data_set_default_string(s,"mediapipe_library","");
    obs_data_set_default_string(s,"yamnet_model","");
    obs_data_set_default_string(s,"yamnet_device","cpu");
}
obs_properties_t* properties(void*){
    auto* props=obs_properties_create();
    obs_properties_add_path(props,"mediapipe_library",
       "MediaPipe Tasks 1.2.3 native library (.so)",OBS_PATH_FILE,
       "Shared library (*.so)",nullptr);
    obs_properties_add_path(props,"yamnet_model","Existing YAMNet (.tflite)",
       OBS_PATH_FILE,"Model (*.tflite)",nullptr);
    auto* device=obs_properties_add_list(props,"yamnet_device","Compute delegate",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(device,"CPU (recommended)","cpu");
    obs_property_list_add_string(device,"Auto (GPU then CPU)","auto");
    obs_property_list_add_string(device,"GPU (experimental; no fallback)","gpu");
    obs_properties_add_text(props,"status_note",
       "Experimental; recreate filter to change model/runtime/delegate. Audio passes "
       "through unmodified. Read-only sound_yamnet_read_v1 procedure exposes "
       "label, score, window count, dropped samples, timing and status. "
       "Selected delegate: sound_yamnet_device_v1. No caption overlay yet.",OBS_TEXT_INFO);
    return props;
}
const char* get_name(void*){return "LLL.sound Native YAMNet (experimental)";}
void* create(obs_data_t* settings,obs_source_t* source){
    auto* audio=obs_get_audio();
    const uint32_t rate=audio?audio_output_get_sample_rate(audio):0;
    const uint32_t channels=audio?audio_output_get_channels(audio):0;
    if((rate!=44100&&rate!=48000)||channels==0||
       channels>MAX_AUDIO_CHANNELS){
        blog(LOG_WARNING,"[Sound/YAMNet] Unsupported OBS sample rate/channels");
        return nullptr;
    }
    std::unique_ptr<YamnetWorker> s;
    try{s=std::make_unique<YamnetWorker>(rate,channels);}
    catch(...){return nullptr;}
    const char* lib=obs_data_get_string(settings,"mediapipe_library");
    const char* model=obs_data_get_string(settings,"yamnet_model");
    if(lib)s->library=lib;
    if(model)s->model=model;
    const std::string device=obs_data_get_string(settings,"yamnet_device");
    s->device=device.empty()?"cpu":device;
    auto* handler=obs_source_get_proc_handler(source);
    if(!handler)return nullptr;
    try{s->worker=std::thread(worker_entry,s.get());}
    catch(...){return nullptr;}
    proc_handler_add(handler,
       "void sound_yamnet_read_v1(out string label, out float score, "
       "out int windows, out int dropped_samples, out int inference_ms, "
       "out int callback_calls, out string status, out bool available)",snapshot,s.get());
    proc_handler_add(handler,"void sound_yamnet_device_v1(out string requested, out string selected, out string status)",device_status,s.get());
    proc_handler_add(handler,"void sound_yamnet_list_v1(in ptr inventory)",list_categories,s.get());
    proc_handler_add(handler,"void sound_yamnet_score_v1(in string category, out float score, out bool available)",category_score,s.get());
    sound_signals::register_broker(source);
    return s.release();
}
void destroy(void* ptr){
    auto* s=static_cast<YamnetWorker*>(ptr);
    if(!s)return;
    s->stopping.store(true,std::memory_order_release);
    if(s->worker.joinable())s->worker.join();
    delete s;
}
obs_audio_data* filter_audio(void* ptr,obs_audio_data* packet){
    auto* s=static_cast<YamnetWorker*>(ptr);
    if(!s||!packet||!packet->frames||
       !s->started.load(std::memory_order_acquire) ||
       s->overloaded.load(std::memory_order_acquire))return packet;
    s->callback_calls.fetch_add(1,std::memory_order_relaxed);
    std::array<const float*,MAX_AUDIO_CHANNELS> channels{};
    for(size_t ch=0;ch<s->channels;++ch){
        if(!packet->data[ch])return packet; // Never invent missing audio.
        channels[ch]=reinterpret_cast<const float*>(packet->data[ch]);
    }
    // Stack scratch only: no locks/allocations/inference/resampling on callback.
    std::array<float,CAPTURE_BLOCK> mono{};
    for(uint32_t offset=0;offset<packet->frames;){
        const size_t n=std::min<size_t>(CAPTURE_BLOCK,packet->frames-offset);
        for(size_t i=0;i<n;++i){
            float value=0;
            for(size_t ch=0;ch<s->channels;++ch)value+=channels[ch][offset+i];
            mono[i]=value/float(s->channels);
        }
        const size_t written=s->ring.write(mono.data(),n);
        if(written!=n){
            s->dropped.fetch_add(int64_t(packet->frames-offset-written));
            // Discontinuity invalidates the stream: do not splice timestamps.
            s->overloaded.store(true,std::memory_order_release);
            break;
        }
        offset+=uint32_t(n);
    }
    return packet;
}
} // namespace
extern "C" {
struct obs_source_info yamnet_classifier_source={
    .id="yamnet_classifier_source",
    .type=OBS_SOURCE_TYPE_FILTER,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=get_name,
    .create=create,
    .destroy=destroy,
    .get_defaults=defaults,
    .get_properties=properties,
    .filter_audio=filter_audio,
};
}