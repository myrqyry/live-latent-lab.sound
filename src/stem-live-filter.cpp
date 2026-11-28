// Opt-in, bounded delayed-live OBS filter backed by the user's existing
// local HTDemucs sidecar. Its real-time callback never does model inference,
// socket I/O, mutex locking, heap allocation or filesystem operations.
#include <obs-module.h>
#include <util/bmem.h>
#include "framework/preferences.hpp"
#include "framework/stem-managed-worker.hpp"
#include <media-io/audio-io.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "framework/live-stem-pipeline.hpp"
#include "framework/stem-window-overlap.hpp"
#include "framework/stem-readout.hpp"
#include "framework/live-stem-client.hpp"
#include "framework/live-stem-dispatch.hpp"
#include "framework/live-stem-outputs.hpp"
#include "framework/rt_wake.hpp"
#include "framework/worker_recovery.hpp"

namespace {
struct StemFilter {
    explicit StemFilter(uint32_t rate,bool mel)
        : sample_rate(rate),melband(mel),
          pipeline(mel?stts::LiveStemMixPipeline(rate,rate*28/10,rate*21/10,2):stts::LiveStemMixPipeline(rate)),
          client(stopping) {}
    const uint32_t sample_rate;
    const bool melband;
    stts::LiveStemMixPipeline pipeline;
    stts::RtWake wake;
    std::atomic<bool> stopping{false};
    stts::LiveStemClient client;
    std::mutex control; // settings/worker only
    // Q12 gains packed into one lock-free atomic, independent of model revision.
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::atomic<uint64_t> mix{stts::pack_stem_gains({0,0,0,1})};
    std::string socket_path,python,model,safetensors,native_model;
    bool native_backend=false;
    bool managed=true;
    std::atomic<uint64_t> activity{0};
    std::atomic<bool> attached{false};
    enum WorkerState{Idle,Loading,Running,Unavailable,External};
    std::atomic<int> worker_state{Idle};
    std::string worker_detail;

    bool smooth_windows=false; // control lock
    uint64_t revision=0;
    enum Status{Buffering,Ready,Error};
    std::atomic<int> status{Buffering};
    std::atomic<uint64_t> windows{0},retries{0};
    std::thread worker;
    obs_source_t* context=nullptr; // valid until both workers join in destroy
    std::string fanout_uuid; // control lock
    std::atomic<bool> fanout_allowed{false},fanout_ready{false};
    stts::StemDispatchQueue dispatch_queue;
    stts::RtWake dispatch_wake;
    std::thread dispatcher;
    std::atomic<uint64_t> dispatch_packets{0},dispatch_drops{0};
    std::atomic<unsigned> dispatch_subscribers{0};
};

const char* get_name(void*) {
    return "LLL.sound Buffered stem (experimental)";
}
constexpr const char* gain_keys[]={"drums_gain","bass_gain","other_gain","vocals_gain"};
constexpr const char* mute_keys[]={"drums_mute","bass_mute","other_mute","vocals_mute"};
constexpr const char* solo_keys[]={"drums_solo","bass_solo","other_solo","vocals_solo"};
constexpr const char* stem_names[]={"Drums","Bass","Other","Vocals"};

void defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings,"worker_mode",-1);
    obs_data_set_default_int(settings,"stem",3);
    obs_data_set_default_int(settings,"mix_mode",0);
    obs_data_set_default_bool(settings,"smooth_windows",false);
    for(size_t i=0;i<4;++i){
        obs_data_set_default_double(settings,gain_keys[i],1.);
        obs_data_set_default_bool(settings,mute_keys[i],false);
        obs_data_set_default_bool(settings,solo_keys[i],false);
    }
    const char* dir=std::getenv("XDG_RUNTIME_DIR");
    if(dir && *dir){
        const std::string path=std::string(dir)+"/lll-sound-stems.sock";
        obs_data_set_default_string(settings,"socket_path",path.c_str());
    }
}
constexpr const char* mel_gain_keys[]={"vocals_gain","instrumental_gain"};
constexpr const char* mel_mute_keys[]={"vocals_mute","instrumental_mute"};
constexpr const char* mel_solo_keys[]={"vocals_solo","instrumental_solo"};
constexpr const char* mel_names[]={"Vocals","Instrumental"};
const char* mel_name(void*){return "LLL.sound RoFormer stems (experimental)";}
void mel_defaults(obs_data_t* settings){
    defaults(settings);
#ifdef SOUND_WITH_STEM_NATIVE
    obs_data_set_default_bool(settings,"native_backend",true);
#else
    obs_data_set_default_bool(settings,"native_backend",false);
#endif
    obs_data_set_default_double(settings,"instrumental_gain",1.);
    obs_data_set_default_bool(settings,"instrumental_mute",false);
    obs_data_set_default_bool(settings,"instrumental_solo",false);
    const char* dir=std::getenv("XDG_RUNTIME_DIR");
    if(dir&&*dir)obs_data_set_default_string(settings,"socket_path",(std::string(dir)+"/lll-sound-melband.sock").c_str());
}
bool managed_mode(obs_data_t* settings,bool melband){
    const auto mode=obs_data_get_int(settings,"worker_mode");
    if(mode>=0)return mode!=1;
    std::string path=obs_data_get_string(settings,"socket_path");
    const char* dir=std::getenv("XDG_RUNTIME_DIR");
    const auto default_path=dir&&*dir?std::string(dir)+(melband?"/lll-sound-melband.sock":"/lll-sound-stems.sock"):std::string{};
    return path.empty()||path==default_path;
}
void update(void* ptr,obs_data_t* settings) {
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
    int selected=int(obs_data_get_int(settings,"stem"));
    int mode=int(obs_data_get_int(settings,"mix_mode"));
    stts::StemGains gains{};unsigned muted=0,solo=0;
    for(size_t i=0;i<(data->melband?2u:4u);++i){
        gains[i]=float(obs_data_get_double(settings,data->melband?mel_gain_keys[i]:gain_keys[i]));
        if(obs_data_get_bool(settings,data->melband?mel_mute_keys[i]:mute_keys[i]))muted|=1u<<i;
        if(obs_data_get_bool(settings,data->melband?mel_solo_keys[i]:solo_keys[i]))solo|=1u<<i;
    }
    const auto mix=stts::pack_stem_gains(data->melband?
        stts::resolve_melband_gains(mode,selected,gains,muted,solo):
        stts::resolve_stem_gains(mode,selected,gains,muted,solo));
    std::string path=obs_data_get_string(settings,"socket_path");
    const bool managed=managed_mode(settings,data->melband);
    std::string python=obs_data_get_string(settings,"worker_python"),model=obs_data_get_string(settings,"worker_model"),
        safetensors=obs_data_get_string(settings,"worker_safetensors");
    const bool native_backend=data->melband&&obs_data_get_bool(settings,"native_backend");
    std::string native_model=obs_data_get_string(settings,"native_model");
    if(native_backend&&native_model.empty()){
        auto root=sound_preferences::directory();
        if(!root.empty())native_model=(std::filesystem::u8path(root)/"Mel-Band-RoFormer-GGUF"/"mel-band-roformer-f16.gguf").string();
    }
    std::lock_guard<std::mutex> lock(data->control);
    data->mix.store(mix,std::memory_order_release);
    const bool smooth=!data->melband&&obs_data_get_bool(settings,"smooth_windows");
    if(!data->revision||path!=data->socket_path||smooth!=data->smooth_windows||managed!=data->managed||
       python!=data->python||model!=data->model||safetensors!=data->safetensors||
       native_backend!=data->native_backend||native_model!=data->native_model){
        if(data->revision||managed)data->pipeline.invalidate();
        data->socket_path=std::move(path);data->smooth_windows=smooth;data->managed=managed;
        data->python=std::move(python);data->model=std::move(model);data->safetensors=std::move(safetensors);
        data->native_backend=native_backend;data->native_model=std::move(native_model);
        data->worker_state=managed?StemFilter::Idle:StemFilter::External;data->worker_detail.clear();
        ++data->revision;data->status=StemFilter::Buffering;data->wake.notify();
    }
}
bool retry_worker(obs_properties_t*,obs_property_t*,void* ptr){
    auto* data=static_cast<StemFilter*>(ptr);if(!data)return false;
    std::lock_guard<std::mutex> lock(data->control);
    data->pipeline.invalidate();++data->revision;
    data->status=StemFilter::Buffering;data->wake.notify();return true;
}
void snapshot(void* ptr,calldata_t* d){
    auto* data=static_cast<StemFilter*>(ptr);
    std::lock_guard<std::mutex> lock(data->control);
    auto status=data->status.load();
    calldata_set_string(d,"status",status==StemFilter::Error?"error":
        !data->pipeline.ok()||status==StemFilter::Buffering?"buffering":"ready");
    calldata_set_int(d,"windows",data->windows.load());
    calldata_set_int(d,"automatic_retries",data->retries.load());
    calldata_set_int(d,"delay_ms",data->melband?2100:12000);
}
bool mix_mode_changed(obs_properties_t* props,obs_property_t*,obs_data_t* settings){
    const auto mode=obs_data_get_int(settings,"mix_mode");
    obs_property_set_visible(obs_properties_get(props,"stem"),mode==0);
    obs_property_set_visible(obs_properties_get(props,"custom_mix"),mode==2);
    return true;
}
const char* worker_state_name(int state){
    switch(state){
    case StemFilter::Idle:return "Waiting for audio";
    case StemFilter::Loading:return "Loading local CUDA model";
    case StemFilter::Running:return "Managed worker ready";
    case StemFilter::External:return "External worker";
    default:return "Worker unavailable";
    }
}
void worker_snapshot(void* ptr,calldata_t* d){
    auto* data=static_cast<StemFilter*>(ptr);
    std::lock_guard<std::mutex> lock(data->control);
    calldata_set_string(d,"state",worker_state_name(data->worker_state.load()));
    calldata_set_string(d,"detail",data->worker_detail.c_str());
}
bool worker_mode_changed(obs_properties_t* props,obs_property_t*,obs_data_t* settings){
    const auto mode=obs_data_get_int(settings,"worker_mode");
    const bool external=mode==1||(mode==-1&&!managed_mode(settings,false)&&!managed_mode(settings,true));
    obs_property_set_visible(obs_properties_get(props,"socket_path"),external);
    const bool native=obs_properties_get(props,"native_backend")&&obs_data_get_bool(settings,"native_backend");
    if(auto* p=obs_properties_get(props,"native_backend"))obs_property_set_visible(p,!external);
    if(auto* p=obs_properties_get(props,"native_model"))obs_property_set_visible(p,!external&&native);
    for(auto key:{"worker_python","worker_model","worker_safetensors"})
        if(auto* p=obs_properties_get(props,key))obs_property_set_visible(p,!external&&!native);
    return true;
}
std::string status_text(StemFilter* data){
    std::lock_guard<std::mutex> lock(data->control);
    auto info=std::string(worker_state_name(data->worker_state.load()));
    if(!data->worker_detail.empty())info+=": "+data->worker_detail;
    return info;
}
bool refresh_worker_status(obs_properties_t* props,obs_property_t*,void* ptr){
    if(!ptr)return false;
    if(auto* p=obs_properties_get(props,"worker_status"))
        obs_property_set_description(p,status_text(static_cast<StemFilter*>(ptr)).c_str());
    return true;
}
obs_properties_t* properties_for(void* ptr,bool melband) {
    auto* props=obs_properties_create();
    auto* worker_mode=obs_properties_add_list(props,"worker_mode","Inference worker",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(worker_mode,"Automatic (OBS manages worker)",-1);
    obs_property_list_add_int(worker_mode,"Managed by OBS",0);
    obs_property_list_add_int(worker_mode,"External socket (advanced)",1);
    obs_property_set_modified_callback(worker_mode,worker_mode_changed);
    obs_properties_add_text(props,"socket_path","Custom external socket (advanced)",OBS_TEXT_DEFAULT);
    auto model_root=sound_preferences::directory();
    auto runtime_root=sound_preferences::directory(true);
    obs_properties_add_path(props,"worker_python","Python interpreter override",OBS_PATH_FILE,"Executables (*);;All files (*)",runtime_root.c_str());
    obs_properties_add_path(props,"worker_model",melband?"Mel-Band model override":"Demucs model directory override",
        melband?OBS_PATH_FILE:OBS_PATH_DIRECTORY,melband?"SafeTensors (*.safetensors);;All files (*)":nullptr,model_root.c_str());
    if(melband)obs_properties_add_path(props,"worker_safetensors","Optional existing safetensors package",OBS_PATH_DIRECTORY,nullptr,runtime_root.c_str());
    if(melband){
        auto* native=obs_properties_add_bool(props,"native_backend","Native CUDA (GGUF; no Python)");
#ifndef SOUND_WITH_STEM_NATIVE
        obs_property_set_enabled(native,false);
#endif
        obs_property_set_modified_callback(native,worker_mode_changed);
        obs_properties_add_path(props,"native_model","GGUF model (empty uses library F16 default)",OBS_PATH_FILE,
            "GGUF (*.gguf);;All files (*)",model_root.c_str());
    }
    obs_properties_add_text(props,"managed_note","Automatic mode loads the configured local model when audio arrives. Overrides are optional with a saved local profile. Cold loading adds silence before the fixed playback delay; no model downloads or CPU fallback.",OBS_TEXT_INFO);
    if(ptr){
        obs_properties_add_text(props,"worker_status",status_text(static_cast<StemFilter*>(ptr)).c_str(),OBS_TEXT_INFO);
        obs_properties_add_button2(props,"refresh_worker_status","Refresh worker status",refresh_worker_status,ptr);
    }
    if(!melband){
    obs_properties_add_bool(props,"smooth_windows","Smooth window boundaries (2 s overlap)");
    obs_properties_add_text(props,"overlap_note",
        "Optional overlapping inference every 6 s instead of 8 s. "
        "Uses more inference time; keeps the 12 s playback delay. Changing this setting re-buffers.",OBS_TEXT_INFO);
    }
    auto* mode=obs_properties_add_list(props,"mix_mode","Output mode",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode,"Single stem",0);
    obs_property_list_add_int(mode,"Instrumental (no vocals)",1);
    obs_property_list_add_int(mode,"Custom mix",2);
    obs_property_list_add_int(mode,"Separate outputs (silent parent)",3);
    obs_property_set_modified_callback(mode,mix_mode_changed);
    auto* list=obs_properties_add_list(props,"stem","Separated stereo stem",
        OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(list,"Vocals",3);
    if(melband)obs_property_list_add_int(list,"Instrumental",4);
    else{
        obs_property_list_add_int(list,"Drums",0);
        obs_property_list_add_int(list,"Bass",1);
        obs_property_list_add_int(list,"Other",2);
    }
    auto* custom=obs_properties_create();
    for(size_t i=0;i<(melband?2u:4u);++i){
        const auto label=std::string(melband?mel_names[i]:stem_names[i]);
        obs_properties_add_float_slider(custom,melband?mel_gain_keys[i]:gain_keys[i],(label+" level (linear)").c_str(),0.,2.,.01);
        obs_properties_add_bool(custom,melband?mel_mute_keys[i]:mute_keys[i],(label+" mute").c_str());
        obs_properties_add_bool(custom,melband?mel_solo_keys[i]:solo_keys[i],(label+" solo").c_str());
    }
    obs_properties_add_group(props,"custom_mix","Stem mixer",OBS_GROUP_NORMAL,custom);
    obs_properties_add_text(props,"delay_note",melband?
        "Experimental 2.1 s fixed delay, 2.8 s context / 700 ms hop / 350 ms lookahead. Uses LLSTEM02 profile 1 CUDA. Vocals and residual instrumental only. Add Stem Output sources for independent routing. Faults mute, retry and restart prefill.":
        "Fixed 12 s delay; first 12 s silent. Uses local 8 s HTDemucs inference. "
        "All four stereo stems share one inference and playback clock. "
         "Mix controls change without rebuffering. Levels are linear; allow headroom when summing. "
        "Add LLL.sound Stem Output sources and choose this filter for independent routing. "
        "Faults mute and retry up to three times; a changed socket starts fresh prefill. "
        "Never plays dry music as a stem.",OBS_TEXT_INFO);
    obs_properties_add_button2(props,"retry_worker","Retry worker",retry_worker,ptr);
    return props;
}

obs_properties_t* properties(void* ptr){return properties_for(ptr,false);}
obs_properties_t* mel_properties(void* ptr){return properties_for(ptr,true);}

// Only one managed instance of each family at a time until a shared
// multi-session GPU scheduler is qualified. External endpoints stay user-owned.
std::mutex managed_slots[2];
void worker_loop(StemFilter* data) {
    const auto frames=data->pipeline.window_frames();
    stts::StemWindowOverlap overlap(frames,8);
    std::unique_ptr<stts::StemReadout> readout;
    if(data->melband)readout=std::make_unique<stts::StemReadout>(frames,
        data->sample_rate*7/10,data->sample_rate*35/100,data->sample_rate*14/100,4);
    stts::WorkerRecovery budget(data->melband?40:4);
    uint64_t attempt_revision=0,seen_activity=0;
    uint32_t seq=0;
    while(!data->stopping.load()){
        std::string path,python,model,safetensors,native_model;uint64_t revision,epoch=0;bool smooth,managed,native_backend;
        {
            std::lock_guard<std::mutex> lock(data->control);
            revision=data->revision;path=data->socket_path;smooth=data->smooth_windows;managed=data->managed;
            python=data->python;model=data->model;safetensors=data->safetensors;
            native_backend=data->native_backend;native_model=data->native_model;
            if(revision!=attempt_revision){budget.reset();attempt_revision=revision;}
            if(managed)data->pipeline.invalidate();
            data->status=StemFilter::Buffering;
            data->worker_state=managed?StemFilter::Idle:StemFilter::External;
            data->worker_detail.clear();
        }
        auto valid_revision=[&]{
            if(data->stopping.load())return false;
            std::lock_guard<std::mutex> lock(data->control);
            return data->revision==revision;
        };
        std::unique_lock<std::mutex> slot(managed_slots[data->melband?1:0],std::defer_lock);
        stts::StemManagedWorker child;
        bool initialized=true,idle=false;
        if(managed){
            // No model load until real audio arrives. Invalid pipeline stays
            // silent throughout cold startup instead of running out of prefill.
            auto has_input=[&]{
                return data->attached.load()&&obs_source_enabled(data->context)&&data->activity.load()!=seen_activity;
            };
            while(valid_revision()&&!has_input()){
                auto ticket=data->wake.sequence();
                if(!has_input())data->wake.wait(ticket,100);
            }
            if(!valid_revision())continue;
            seen_activity=data->activity.load();
            data->worker_state=StemFilter::Loading;
            char* file=obs_module_file(native_backend?"stem-native/sound-stem-native":"stem-worker/sound-stems.py");
            std::string script=file?file:"";bfree(file);
            bool acquired=slot.try_lock();
            bool spawned=acquired&&(native_backend?child.start_native(script,native_model):
                child.start(script,data->melband?"melband":"demucs",python,model,safetensors));
            initialized=spawned&&child.wait_ready(valid_revision,std::chrono::seconds(60));
            if(!valid_revision())continue;
            if(initialized){
                path=child.path();
                std::lock_guard<std::mutex> lock(data->control);
                data->worker_detail=native_backend?"Native CUDA GGUF":"Python CUDA compatibility";
                data->worker_state=StemFilter::Running;
            }
            else{
                std::lock_guard<std::mutex> lock(data->control);
                data->worker_state=StemFilter::Unavailable;
                data->worker_detail=acquired?(native_backend?"Native CUDA worker unavailable. Check GGUF model/runtime and OBS log.":"Local worker could not load. Check Python/model paths and OBS log."):"Another managed filter owns this model family.";
                auto detail=child.error();
                blog(LOG_WARNING,"[Sound stems] %s %s",data->worker_detail.c_str(),detail.c_str());
            }
        }
        {
            std::lock_guard<std::mutex> lock(data->control);
            if(data->revision!=revision||data->stopping.load())continue;
            if(initialized){
                if(!data->pipeline.ok()&&!data->pipeline.restart())return;
                epoch=data->pipeline.epoch();
            }
        }
        if(readout)readout->reset();else overlap.reset(smooth?size_t(data->sample_rate)*2:0);
        auto current=[&]{return data->pipeline.current(epoch);};
        auto activity=data->activity.load();
        auto last=std::chrono::steady_clock::now();
        while(initialized&&!data->stopping.load()&&current()){
            const auto ticket=data->wake.sequence();
            auto now_activity=data->activity.load();
            if(now_activity!=activity){activity=now_activity;last=std::chrono::steady_clock::now();}
            if(managed){
                if(!child.alive()){data->pipeline.fault(epoch);break;}
                if(std::chrono::steady_clock::now()-last>std::chrono::seconds(30)){
                    idle=true;seen_activity=activity;data->pipeline.fault(epoch);break;
                }
            }
            auto capture=[&](float* l,float* r,size_t n){return data->pipeline.read_input_frames(l,r,n);};
            if(!data->pipeline.worker_prepare()||
               !(readout?readout->capture(capture):overlap.capture(capture))){
                if(!data->stopping.load()&&current())data->wake.wait(ticket,100);
                continue;
            }
            auto publish=[&](const float* out,size_t samples,size_t n){
                return data->pipeline.push_output_frames(out,samples,n);
            };
            auto publish_batch=[&](const float* batch,size_t count){
                return readout?readout->publish(batch,count,publish):overlap.publish(batch,count,publish);
            };
            bool exchanged=readout?
                data->client.exchange_melband(path,data->sample_rate,seq++,readout->left(),readout->right(),frames,current,publish_batch):
                data->client.exchange_batch(path,data->sample_rate,seq++,overlap.left(),overlap.right(),frames,current,publish_batch);
            if(!exchanged){data->pipeline.fault(epoch);break;}
            {
                std::lock_guard<std::mutex> lock(data->control);
                if(data->revision!=revision||!current())break;
                ++data->windows;data->status=StemFilter::Ready;budget.on_success();
            }
        }
        child.stop();
        if(slot.owns_lock())slot.unlock();
        if(data->stopping.load())return;
        if(idle)continue;
        bool retry=false;
        {
            std::lock_guard<std::mutex> lock(data->control);
            if(data->revision!=revision){seen_activity=data->activity.load();continue;}
            data->pipeline.invalidate();data->status=StemFilter::Error;
            if(managed){data->worker_state=StemFilter::Unavailable;if(data->worker_detail.empty())data->worker_detail="Stream interrupted; retrying local worker.";}
            budget.on_fault();retry=budget.can_retry(true);
        }
        blog(LOG_WARNING,"[Sound stems] Stream invalidated; %s",
             retry?"silent recovery pending":"retry budget exhausted; Retry worker to restart");
        auto until=std::chrono::steady_clock::now()+
            std::chrono::milliseconds(retry?budget.delay_ms():0);
        while(!data->stopping.load()){
            const auto ticket=data->wake.sequence();
            {
                std::lock_guard<std::mutex> lock(data->control);
                if(data->revision!=revision)break;
                if(retry&&std::chrono::steady_clock::now()>=until){
                    budget.on_retry();++data->retries;break;
                }
            }
            if(!data->stopping.load())data->wake.wait(ticket,100);
        }
    }
}

void worker_entry(StemFilter* data){
    try{worker_loop(data);}
    catch(...){
        data->pipeline.invalidate();data->status=StemFilter::Error;
        blog(LOG_WARNING,"[Sound stems] Unexpected worker failure; muted");
    }
}

void dispatch_loop(StemFilter* data){
    namespace outputs=stts::stem_outputs;
    std::vector<std::shared_ptr<outputs::Binding>> bindings;
    std::string last_key;
    uint64_t last_revision=0;
    std::array<float,stts::StemDispatchPacket::max_frames> silence{};
    while(!data->stopping.load(std::memory_order_acquire)){
        auto ticket=data->dispatch_wake.sequence();
        std::string key;
        if(data->fanout_allowed.load(std::memory_order_acquire)){
            std::lock_guard<std::mutex> lock(data->control);
            key=data->fanout_uuid;
        }
        const auto revision=outputs::revision.load(std::memory_order_acquire);
        if(key!=last_key||revision!=last_revision){
            bindings=outputs::find(key);
            bindings.erase(std::remove_if(bindings.begin(),bindings.end(),[&](const auto& b){return stts::stem_output_index(data->melband,b->stem)<0;}),bindings.end());
            last_key=std::move(key);last_revision=revision;
        }
        const bool allowed=!bindings.empty()&&data->fanout_allowed.load(std::memory_order_acquire)&&
            obs_source_enabled(data->context);
        data->fanout_ready.store(allowed,std::memory_order_release);
        data->dispatch_subscribers.store(allowed?unsigned(bindings.size()):0,std::memory_order_relaxed);
        if(const auto* packet=data->dispatch_queue.peek()){
            if(allowed){
                // All subscribers receive the same timestamp and frame count.
                // A faulted or obsolete packet can only deliver silence.
                bool valid=data->pipeline.current(packet->epoch);
                for(auto& binding:bindings){
                    if(!binding->live.load(std::memory_order_acquire))continue;
                    auto* source=obs_weak_source_get_source(binding->weak);
                    if(!source)continue;
                    if(!obs_source_removed(source)&&obs_source_enabled(source)&&
                        binding->live.load(std::memory_order_acquire)){
                        obs_source_audio audio{};
                        auto stem=size_t(stts::stem_output_index(data->melband,binding->stem));
                        auto* l=valid?packet->planes[stem*2].data():silence.data();
                        auto* r=valid?packet->planes[stem*2+1].data():silence.data();
                        audio.data[0]=reinterpret_cast<const uint8_t*>(l);
                        audio.data[1]=reinterpret_cast<const uint8_t*>(r);
                        audio.frames=packet->frames;audio.speakers=SPEAKERS_STEREO;
                        audio.format=AUDIO_FORMAT_FLOAT_PLANAR;audio.samples_per_sec=data->sample_rate;
                        audio.timestamp=packet->timestamp;
                        obs_source_output_audio(source,&audio);
                    }
                    obs_source_release(source);
                }
                ++data->dispatch_packets;
            }
            data->dispatch_queue.consume();
        }else data->dispatch_wake.wait(ticket,100);
    }
    data->fanout_ready.store(false,std::memory_order_release);
}
void dispatch_entry(StemFilter* data){
    try{dispatch_loop(data);}
    catch(...){
        data->fanout_ready.store(false,std::memory_order_release);
        data->pipeline.invalidate();data->wake.notify();
        blog(LOG_WARNING,"[Sound stems] Output dispatcher failed; independent outputs stopped");
    }
}
void filter_add(void* ptr,obs_source_t* parent){
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
    const char* uuid=obs_source_get_uuid(data->context);
    {
        std::lock_guard<std::mutex> lock(data->control);
        data->fanout_uuid=uuid?uuid:"";
    }
    data->fanout_allowed.store(uuid&&*uuid&&stts::stem_outputs::allowed_parent(parent),std::memory_order_release);
    data->attached.store(true,std::memory_order_release);
    data->wake.notify();data->dispatch_wake.notify();
}
void filter_remove(void* ptr,obs_source_t*){
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
    data->attached.store(false,std::memory_order_release);
    data->fanout_allowed.store(false,std::memory_order_release);
    data->fanout_ready.store(false,std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(data->control);
        data->pipeline.invalidate();++data->revision;data->status=StemFilter::Buffering;
    }
    data->dispatch_wake.notify();data->wake.notify();
}
void enabled_changed(void* ptr,calldata_t*){
    auto* data=static_cast<StemFilter*>(ptr);
    // Explicit disable/enable is a control discontinuity, not a failed
    // inference retry. No captured audio may survive across this boundary.
    {
        std::lock_guard<std::mutex> lock(data->control);
        data->pipeline.invalidate();++data->revision;data->status=StemFilter::Buffering;
    }
    data->wake.notify();data->dispatch_wake.notify();
}
void fanout_snapshot(void* ptr,calldata_t* d){
    auto* data=static_cast<StemFilter*>(ptr);
    calldata_set_int(d,"packets",data->dispatch_packets.load());
    calldata_set_int(d,"drops",data->dispatch_drops.load());
    calldata_set_int(d,"subscribers",data->dispatch_subscribers.load());
}
void* create_for(obs_data_t* settings,obs_source_t* source,bool melband) {
    auto* audio=obs_get_audio();
    const uint32_t rate=audio?audio_output_get_sample_rate(audio):0;
    if(rate!=48000 && rate!=44100){
        blog(LOG_WARNING,"[Sound stems] Requires OBS stereo project at 44.1 or 48 kHz");
        return nullptr;
    }
    StemFilter* data=nullptr;
    try{data=new StemFilter(rate,melband);}catch(...){return nullptr;}
    if(!data)return nullptr;
    data->context=source;
    try {
        update(data,settings);
        data->worker=std::thread(worker_entry,data);
        data->dispatcher=std::thread(dispatch_entry,data);
    } catch(...) {
        data->stopping.store(true,std::memory_order_release);
        data->wake.notify();data->dispatch_wake.notify();data->client.cancel();
        if(data->worker.joinable())data->worker.join();
        if(data->dispatcher.joinable())data->dispatcher.join();
        delete data;return nullptr;
    }
    proc_handler_add(obs_source_get_proc_handler(source),
        melband?"void sound_stem_live_v2(out string status, out int windows, out int automatic_retries, out int delay_ms)":
        "void sound_stem_live_v1(out string status, out int windows, out int automatic_retries, out int delay_ms)",snapshot,data);
    proc_handler_add(obs_source_get_proc_handler(source),
        "void sound_stem_worker_v1(out string state, out string detail)",worker_snapshot,data);
    proc_handler_add(obs_source_get_proc_handler(source),
        "void sound_stem_fanout_v1(out int packets, out int drops, out int subscribers)",fanout_snapshot,data);
    signal_handler_connect(obs_source_get_signal_handler(source),"enable",enabled_changed,data);
    blog(LOG_INFO,"[Sound stems] Experimental stereo delayed filter created (rate=%u, delay=%dms)",rate,melband?2100:12000);
    return data;
}
void* create(obs_data_t* s,obs_source_t* p){return create_for(s,p,false);}
void* mel_create(obs_data_t* s,obs_source_t* p){return create_for(s,p,true);}
void destroy(void* ptr) {
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
    signal_handler_disconnect(obs_source_get_signal_handler(data->context),"enable",enabled_changed,data);
    data->stopping.store(true,std::memory_order_release);
    data->wake.notify();data->dispatch_wake.notify();
    data->client.cancel(); // interrupt the worker, never an OBS audio call
    if(data->worker.joinable())data->worker.join();
    if(data->dispatcher.joinable())data->dispatcher.join();
    delete data;
}
obs_audio_data* filter_audio(void* ptr,obs_audio_data* packet){
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data || !packet)return packet;
    auto zero=[&]{
        for(size_t ch=0;ch<MAX_AUDIO_CHANNELS;++ch)
            if(packet->data[ch])
                std::fill_n(reinterpret_cast<float*>(packet->data[ch]),
                            packet->frames,0.f);
    };
    if(!packet->frames)return packet;
    data->activity.fetch_add(1,std::memory_order_relaxed);
    if(!data->pipeline.ok())data->wake.notify();
    if(!packet->data[0] || !packet->data[1]){
        const auto epoch=data->pipeline.epoch();
        data->pipeline.fault(epoch);
        if(epoch&1)data->wake.notify();
        zero();return packet;
    }
    auto* left=reinterpret_cast<float*>(packet->data[0]);
    auto* right=reinterpret_cast<float*>(packet->data[1]);
    const bool was_valid=data->pipeline.ok();
    auto gains=stts::unpack_stem_gains(data->mix.load(std::memory_order_acquire));
    if(data->fanout_ready.load(std::memory_order_acquire)&&data->fanout_allowed.load(std::memory_order_acquire)){
        size_t offset=0;
        while(offset<packet->frames){
            auto* out=data->dispatch_queue.reserve();
            if(!out){
                ++data->dispatch_drops;
                data->pipeline.invalidate();
                data->pipeline.process_mix(left+offset,right+offset,packet->frames-offset,gains);
                data->dispatch_wake.notify();
                break;
            }
            const auto n=std::min<size_t>(packet->frames-offset,stts::StemDispatchPacket::max_frames);
            out->epoch=data->pipeline.epoch();
            out->timestamp=packet->timestamp+uint64_t(offset)*1000000000ULL/data->sample_rate;
            out->frames=uint32_t(n);
            std::array<float*,8> planes{};
            for(size_t ch=0;ch<8;++ch)planes[ch]=out->planes[ch].data();
            data->pipeline.process_mix_and_stems(left+offset,right+offset,n,gains,planes);
            data->dispatch_queue.publish();data->dispatch_wake.notify();
            offset+=n;
        }
    }else data->pipeline.process_mix(left,right,packet->frames,gains);
    if((was_valid&&!data->pipeline.ok())||data->pipeline.input_ready())data->wake.notify();
    for(size_t ch=2;ch<MAX_AUDIO_CHANNELS;++ch)
        if(packet->data[ch])
            std::fill_n(reinterpret_cast<float*>(packet->data[ch]),packet->frames,0.f);
    return packet;
}
} // namespace

extern "C" {
struct obs_source_info stem_live_filter={
    .id="stem_live_buffered_filter",
    .type=OBS_SOURCE_TYPE_FILTER,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=get_name,
    .create=create,
    .destroy=destroy,
    .get_defaults=defaults,
    .get_properties=properties,
    .update=update,
    .filter_audio=filter_audio,
    .filter_remove=filter_remove,
    .filter_add=filter_add,
};
struct obs_source_info stem_live_melband_filter={
    .id=stts::stem_outputs::mel_filter_id,
    .type=OBS_SOURCE_TYPE_FILTER,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=mel_name,.create=mel_create,.destroy=destroy,
    .get_defaults=mel_defaults,.get_properties=mel_properties,.update=update,
    .filter_audio=filter_audio,.filter_remove=filter_remove,.filter_add=filter_add,
};
struct obs_source_info stem_live_output={
    .id=stts::stem_outputs::source_id,
    .type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=stts::stem_outputs::name,
    .create=stts::stem_outputs::create,
    .destroy=stts::stem_outputs::destroy,
    .get_defaults=stts::stem_outputs::defaults,
    .get_properties=stts::stem_outputs::properties,
    .update=stts::stem_outputs::update,
};
}
