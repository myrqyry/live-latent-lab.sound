#include "framework/preferences.hpp"
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
#include "framework/classifier_capture.hpp"
#include "framework/worker_recovery.hpp"
#include "framework/rt_wake.hpp"

#include "interop/sound_signal_client.hpp"
namespace {
constexpr uint32_t TARGET_HZ=16000;
constexpr size_t WINDOW=TARGET_HZ,READ_BLOCK=4096,CAPTURE_BLOCK=1024;
struct Config {
    std::string library,model,device="cpu";
    bool operator==(const Config& o)const{
        return library==o.library&&model==o.model&&device==o.device;
    }
};
struct YamnetWorker {
    YamnetWorker(uint32_t rate,uint32_t channel_count)
        : sample_rate(rate),channels(channel_count),capture(size_t(rate)*3) {}
    const uint32_t sample_rate,channels;
    stts::ClassifierCapture capture;
    stts::RtWake wake;
    Config config;
    uint64_t revision=0,result_epoch=0;
    std::string delegate;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> retries{0};
    std::atomic<int64_t> dropped{0},callback_calls{0};
    std::thread worker;
    std::mutex snapshot_mutex; // Worker and read-only procedure; never callback.
    std::string status="loading",label;
    double score=0;
    std::vector<stts::mp_audio_123::Prediction> categories;
    int64_t windows=0,inference_ms=0;
    std::chrono::steady_clock::time_point last_result{};
};
// All result/config fields use snapshot_mutex. Invalidation of capture is
// atomic and does not require that lock on the callback.
void clear_result(YamnetWorker* s){
    s->result_epoch=0;s->label.clear();s->score=0;s->categories.clear();
    s->last_result={};s->inference_ms=0;
}
bool result_available(YamnetWorker* s){
    return s->status=="ready"&&s->capture.current(s->result_epoch)&&
        std::chrono::steady_clock::now()-s->last_result<=std::chrono::milliseconds(2500);
}
void run_worker(YamnetWorker* s) {
    stts::WorkerRecovery budget(30);
    uint64_t attempt_revision=0;
    while(!s->stopping.load(std::memory_order_acquire)){
        Config config;uint64_t revision;
        {
            std::lock_guard<std::mutex> lock(s->snapshot_mutex);
            config=s->config;revision=s->revision;
            s->status="loading";s->delegate.clear();clear_result(s);
        }
        if(revision!=attempt_revision){attempt_revision=revision;budget.reset();}
        bool initialized=false;
        const auto dropped_before=s->dropped.load();
        try{
            stts::mp_audio_123::Classifier classifier;
            std::string error;
            if(!classifier.open(config.library,config.model,error,1024,config.device))
                throw std::runtime_error("Cannot open native classifier: "+error);
            const resample_info input_info={s->sample_rate,AUDIO_FORMAT_FLOAT_PLANAR,SPEAKERS_MONO};
            const resample_info output_info={TARGET_HZ,AUDIO_FORMAT_FLOAT_PLANAR,SPEAKERS_MONO};
            std::unique_ptr<audio_resampler_t,decltype(&audio_resampler_destroy)> resampler(
                audio_resampler_create(&output_info,&input_info),audio_resampler_destroy);
            if(!resampler)throw std::runtime_error("OBS audio resampler unavailable");
            std::array<float,READ_BLOCK> incoming{};
            std::vector<float> window(WINDOW);
            size_t used=0;uint64_t epoch=0;
            {
                std::lock_guard<std::mutex> lock(s->snapshot_mutex);
                if(s->stopping.load())return;
                if(revision!=s->revision)continue;
                epoch=s->capture.activate();
                if(!epoch)throw std::runtime_error("Capture epoch unavailable");
                initialized=true;s->status="ready";s->delegate=classifier.delegate();
            }
            blog(LOG_INFO,"[Sound/YAMNet] native classifier ready (%u Hz), requested=%s delegate=%s",
                 s->sample_rate,config.device.c_str(),classifier.delegate().c_str());
            while(!s->stopping.load()&&s->capture.current(epoch)){
                const auto ticket=s->wake.sequence();
                const auto count=s->capture.prepare(epoch)?
                    s->capture.read(epoch,incoming.data(),incoming.size()):0;
                if(!count){
                    if(!s->stopping.load()&&s->capture.current(epoch))
                        s->wake.wait(ticket,stts::RtWake::event_driven?100:4);
                    continue;
                }
                const uint8_t* input[MAX_AUDIO_CHANNELS]{};
                uint8_t* output[MAX_AUDIO_CHANNELS]{};
                input[0]=reinterpret_cast<const uint8_t*>(incoming.data());
                uint32_t frames=0;uint64_t offset=0;
                if(!audio_resampler_resample(resampler.get(),output,&frames,&offset,
                                              input,uint32_t(count))||(frames&&!output[0]))
                    throw std::runtime_error("OBS resampler failed");
                const auto* samples=reinterpret_cast<const float*>(output[0]);
                for(uint32_t i=0;i<frames;++i){
                    if(s->stopping.load()||!s->capture.current(epoch))break;
                    window[used++]=samples[i];
                    if(used!=WINDOW)continue;
                    used=0;
                    std::vector<stts::mp_audio_123::Prediction> result;
                    error.clear();
                    const auto begin=std::chrono::steady_clock::now();
                    const bool ok=classifier.classify(window.data(),WINDOW,TARGET_HZ,result,error);
                    const auto duration=std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()-begin).count();
                    if(!ok||result.empty())throw std::runtime_error("Native inference failed: "+error);
                    std::lock_guard<std::mutex> lock(s->snapshot_mutex);
                    if(s->stopping.load()||revision!=s->revision||!s->capture.current(epoch))break;
                    s->label=result.front().category;s->score=result.front().score;
                    s->categories=std::move(result);s->inference_ms=duration;
                    s->last_result=std::chrono::steady_clock::now();s->result_epoch=epoch;
                    ++s->windows;budget.on_success();
                }
            }
        }catch(const std::exception& e){
            if(!s->stopping.load())blog(LOG_WARNING,"[Sound/YAMNet] %s",e.what());
        }catch(...){
            if(!s->stopping.load())blog(LOG_WARNING,"[Sound/YAMNet] Unknown worker error");
        }
        if(s->stopping.load())return;
        bool retry=false;
        {
            std::lock_guard<std::mutex> lock(s->snapshot_mutex);
            if(revision!=s->revision)continue;
            s->capture.pause();clear_result(s);s->delegate.clear();
            s->status=s->dropped.load()>dropped_before?"overload":"error";
            budget.on_fault();retry=budget.can_retry(initialized);
        }
        const auto until=std::chrono::steady_clock::now()+
            std::chrono::milliseconds(retry?budget.delay_ms():0);
        while(!s->stopping.load()){
            const auto ticket=s->wake.sequence();
            {
                std::lock_guard<std::mutex> lock(s->snapshot_mutex);
                if(revision!=s->revision)break;
                if(retry&&std::chrono::steady_clock::now()>=until){
                    budget.on_retry();++s->retries;break;
                }
            }
            if(!s->stopping.load())s->wake.wait(ticket,100);
        }
    }
}
void worker_entry(YamnetWorker* s){
    try{run_worker(s);}
    catch(...){
        std::lock_guard<std::mutex> lock(s->snapshot_mutex);
        s->capture.pause();clear_result(s);s->status="error";s->delegate.clear();
    }
}
void snapshot(void* ptr,calldata_t* output){
    auto* s=static_cast<YamnetWorker*>(ptr);
    if(!s)return;
    std::lock_guard<std::mutex> lock(s->snapshot_mutex);
    // A stopped microphone cannot leave a previous sound classification
    // looking current forever. This clock check runs only on procedure reads.
    // One epoch snapshot linearizes the returned status, label and score.
    const auto epoch=s->capture.epoch();
    const bool current=(epoch&1)&&epoch==s->result_epoch;
    const bool expired=current&&
       std::chrono::steady_clock::now()-s->last_result>std::chrono::milliseconds(2500);
    const bool available=s->status=="ready"&&current&&!expired;
    calldata_set_string(output,"label",available?s->label.c_str():"");
    calldata_set_float(output,"score",available?s->score:0.);
    calldata_set_int(output,"windows",s->windows);
    calldata_set_int(output,"dropped_samples",s->dropped.load());
    calldata_set_int(output,"inference_ms",s->inference_ms);
    calldata_set_int(output,"callback_calls",s->callback_calls.load());
    calldata_set_string(output,"status",
        s->status=="ready"&&!(epoch&1)?"overload":
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
    {std::lock_guard<std::mutex> lock(s->snapshot_mutex);
     if(s->capture.current(s->result_epoch))copy=s->categories;}
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
        if(result_available(s)){
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
    calldata_set_string(d,"requested",s->config.device.c_str());
    calldata_set_string(d,"selected",s->delegate.c_str());
    calldata_set_string(d,"status",s->status=="ready"&&!(s->capture.epoch()&1)?"overload":s->status.c_str());
}
void defaults(obs_data_t* s){
    obs_data_set_default_string(s,"mediapipe_library","");
    obs_data_set_default_string(s,"yamnet_model","");
    obs_data_set_default_string(s,"yamnet_device","cpu");
}
void update(void* ptr,obs_data_t* settings){
    auto* s=static_cast<YamnetWorker*>(ptr);if(!s)return;
    Config next{obs_data_get_string(settings,"mediapipe_library"),
        obs_data_get_string(settings,"yamnet_model"),obs_data_get_string(settings,"yamnet_device")};
    if(next.device.empty())next.device="cpu";
    std::lock_guard<std::mutex> lock(s->snapshot_mutex);
    if(!s->revision||!(next==s->config)){
        s->capture.pause();s->config=std::move(next);++s->revision;
        clear_result(s);s->delegate.clear();s->status="loading";s->wake.notify();
    }
}
bool retry_models(obs_properties_t*,obs_property_t*,void* ptr){
    auto* s=static_cast<YamnetWorker*>(ptr);if(!s)return false;
    std::lock_guard<std::mutex> lock(s->snapshot_mutex);
    s->capture.pause();++s->revision;clear_result(s);
    s->delegate.clear();s->status="loading";s->wake.notify();return true;
}
void recovery_snapshot(void* ptr,calldata_t* call){
    auto* s=static_cast<YamnetWorker*>(ptr);
    calldata_set_int(call,"automatic_retries",s->retries.load());
}
obs_properties_t* properties(void* ptr){
    auto* props=obs_properties_create();
    obs_properties_add_path(props,"mediapipe_library",
       "MediaPipe Tasks 1.2.3 native library (.so)",OBS_PATH_FILE,
       "Shared library (*.so)",sound_preferences::directory(true).c_str());
    obs_properties_add_path(props,"yamnet_model","Existing YAMNet (.tflite)",
       OBS_PATH_FILE,"Model (*.tflite)",sound_preferences::directory(false).c_str());
    auto* device=obs_properties_add_list(props,"yamnet_device","Compute delegate",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(device,"CPU (recommended)","cpu");
    obs_property_list_add_string(device,"Auto (GPU then CPU)","auto");
    obs_property_list_add_string(device,"GPU (experimental; no fallback)","gpu");
    obs_properties_add_text(props,"status_note",
       "Experimental; model/runtime/delegate reload live. Bounded recovery after faults. Audio passes "
       "through unmodified. Read-only sound_yamnet_read_v1 procedure exposes "
       "label, score, window count, dropped samples, timing and status. "
       "Selected delegate: sound_yamnet_device_v1. No caption overlay yet.",OBS_TEXT_INFO);
    obs_properties_add_button2(props,"retry_models","Retry models",retry_models,ptr);
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
    update(s.get(),settings);
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
    proc_handler_add(handler,"void sound_yamnet_recovery_v1(out int automatic_retries)",recovery_snapshot,s.get());
    sound_signals::register_broker(source);
    return s.release();
}
void destroy(void* ptr){
    auto* s=static_cast<YamnetWorker*>(ptr);
    if(!s)return;
    s->stopping.store(true,std::memory_order_release);
    s->wake.notify();
    if(s->worker.joinable())s->worker.join();
    delete s;
}
obs_audio_data* filter_audio(void* ptr,obs_audio_data* packet){
    auto* s=static_cast<YamnetWorker*>(ptr);
    if(!s||!packet||!packet->frames)return packet;
    auto epoch=s->capture.begin_packet();
    if(!epoch){
        if(s->capture.worker_ready())s->wake.notify();
        return packet;
    }
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
        if(!s->capture.current(epoch))break;
        const size_t written=s->capture.write(epoch,mono.data(),n);
        if(written!=n){
            s->dropped.fetch_add(int64_t(packet->frames-offset-written));
            // Discontinuity invalidates the stream: do not splice timestamps.
            s->capture.fault(epoch);
            break;
        }
        offset+=uint32_t(n);
    }
    s->wake.notify();
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
    .update=update,
    .filter_audio=filter_audio,
};
}