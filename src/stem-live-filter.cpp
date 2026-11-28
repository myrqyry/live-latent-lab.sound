// Opt-in, bounded delayed-live OBS filter backed by the user's existing
// local HTDemucs sidecar. Its real-time callback never does model inference,
// socket I/O, mutex locking, heap allocation or filesystem operations.
#include <obs-module.h>
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
#include "framework/live-stem-client.hpp"
#include "framework/live-stem-dispatch.hpp"
#include "framework/live-stem-outputs.hpp"
#include "framework/rt_wake.hpp"
#include "framework/worker_recovery.hpp"

namespace {
struct StemFilter {
    explicit StemFilter(uint32_t rate)
        : sample_rate(rate),pipeline(rate),client(stopping) {}
    uint32_t sample_rate;
    stts::LiveStemMixPipeline pipeline;
    stts::RtWake wake;
    std::atomic<bool> stopping{false};
    stts::LiveStemClient client;
    std::mutex control; // settings/worker only
    // Q12 gains packed into one lock-free atomic, independent of model revision.
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::atomic<uint64_t> mix{stts::pack_stem_gains({0,0,0,1})};
    std::string socket_path;
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
void update(void* ptr,obs_data_t* settings) {
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
    int selected=int(obs_data_get_int(settings,"stem"));
    int mode=int(obs_data_get_int(settings,"mix_mode"));
    stts::StemGains gains{};unsigned muted=0,solo=0;
    for(size_t i=0;i<4;++i){
        gains[i]=float(obs_data_get_double(settings,gain_keys[i]));
        if(obs_data_get_bool(settings,mute_keys[i]))muted|=1u<<i;
        if(obs_data_get_bool(settings,solo_keys[i]))solo|=1u<<i;
    }
    const auto mix=stts::pack_stem_gains(stts::resolve_stem_gains(mode,selected,gains,muted,solo));
    std::string path=obs_data_get_string(settings,"socket_path");
    std::lock_guard<std::mutex> lock(data->control);
    data->mix.store(mix,std::memory_order_release);
    const bool smooth=obs_data_get_bool(settings,"smooth_windows");
    if(!data->revision||path!=data->socket_path||smooth!=data->smooth_windows){
        if(data->revision)data->pipeline.invalidate();
        data->socket_path=std::move(path);data->smooth_windows=smooth;
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
    calldata_set_int(d,"delay_ms",12000);
}
bool mix_mode_changed(obs_properties_t* props,obs_property_t*,obs_data_t* settings){
    const auto mode=obs_data_get_int(settings,"mix_mode");
    obs_property_set_visible(obs_properties_get(props,"stem"),mode==0);
    obs_property_set_visible(obs_properties_get(props,"custom_mix"),mode==2);
    return true;
}
obs_properties_t* properties(void* ptr) {
    auto* props=obs_properties_create();
    obs_properties_add_text(props,"socket_path",
        "Local worker socket (live reload)",OBS_TEXT_DEFAULT);
    obs_properties_add_bool(props,"smooth_windows","Smooth window boundaries (2 s overlap)");
    obs_properties_add_text(props,"overlap_note",
        "Optional overlapping inference every 6 s instead of 8 s. "
        "Uses more inference time; keeps the 12 s playback delay. Changing this setting re-buffers.",OBS_TEXT_INFO);
    auto* mode=obs_properties_add_list(props,"mix_mode","Output mode",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode,"Single stem",0);
    obs_property_list_add_int(mode,"Instrumental (no vocals)",1);
    obs_property_list_add_int(mode,"Custom mix",2);
    obs_property_list_add_int(mode,"Separate outputs (silent parent)",3);
    obs_property_set_modified_callback(mode,mix_mode_changed);
    auto* list=obs_properties_add_list(props,"stem","Separated stereo stem",
        OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(list,"Vocals",3);
    obs_property_list_add_int(list,"Drums",0);
    obs_property_list_add_int(list,"Bass",1);
    obs_property_list_add_int(list,"Other",2);
    auto* custom=obs_properties_create();
    for(size_t i=0;i<4;++i){
        const auto label=std::string(stem_names[i]);
        obs_properties_add_float_slider(custom,gain_keys[i],(label+" level (linear)").c_str(),0.,2.,.01);
        obs_properties_add_bool(custom,mute_keys[i],(label+" mute").c_str());
        obs_properties_add_bool(custom,solo_keys[i],(label+" solo").c_str());
    }
    obs_properties_add_group(props,"custom_mix","Stem mixer",OBS_GROUP_NORMAL,custom);
    obs_properties_add_text(props,"delay_note",
        "Fixed 12 s delay; first 12 s silent. Needs local 8 s HTDemucs worker. "
        "All four stereo stems share one inference and playback clock. "
         "Mix controls change without rebuffering. Levels are linear; allow headroom when summing. "
        "Add LLL.sound Stem Output sources and choose this filter for independent routing. "
        "Faults mute and retry up to three times; a changed socket starts fresh prefill. "
        "Never plays dry music as a stem.",OBS_TEXT_INFO);
    obs_properties_add_button2(props,"retry_worker","Retry worker",retry_worker,ptr);
    return props;
}

void worker_loop(StemFilter* data) {
    const auto frames=data->pipeline.window_frames();
    stts::StemWindowOverlap overlap(frames,8);
    // Four healthy model responses replenish the shared retry budget.
    // The external sidecar is user-owned: reconnect, never spawn or kill it.
    stts::WorkerRecovery budget(4);
    uint64_t attempt_revision=0;
    uint32_t seq=0;
    while(!data->stopping.load()){
        std::string path;uint64_t revision,epoch;bool smooth;
        {
            std::lock_guard<std::mutex> lock(data->control);
            revision=data->revision;path=data->socket_path;smooth=data->smooth_windows;
            if(revision!=attempt_revision){budget.reset();attempt_revision=revision;}
            if(!data->pipeline.ok()&&!data->pipeline.restart())return;
            epoch=data->pipeline.epoch();data->status=StemFilter::Buffering;
        }
        overlap.reset(smooth?size_t(data->sample_rate)*2:0);
        auto current=[&]{return data->pipeline.current(epoch);};
        while(!data->stopping.load()&&current()){
            const auto ticket=data->wake.sequence();
            if(!data->pipeline.worker_prepare()||
               !overlap.capture([&](float* l,float* r,size_t n){
                   return data->pipeline.read_input_frames(l,r,n);
               })){
                if(!data->stopping.load()&&current())data->wake.wait(ticket,100);
                continue;
            }
            if(!data->client.exchange_batch(path,data->sample_rate,seq++,
                    overlap.left(),overlap.right(),frames,current,
                    [&](const float* batch,size_t count){
                        return overlap.publish(batch,count,[&](const float* out,size_t samples,size_t n){
                            return data->pipeline.push_output_frames(out,samples,n);
                        });
                    })){
                data->pipeline.fault(epoch);break;
            }
            {
                std::lock_guard<std::mutex> lock(data->control);
                if(data->revision!=revision||!current())break;
                ++data->windows;data->status=StemFilter::Ready;budget.on_success();
            }
        }
        if(data->stopping.load())return;
        bool retry=false;
        {
            std::lock_guard<std::mutex> lock(data->control);
            if(data->revision!=revision)continue;
            data->pipeline.fault(epoch);data->status=StemFilter::Error;
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
            bindings=outputs::find(key);last_key=std::move(key);last_revision=revision;
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
                        auto stem=size_t(binding->stem);
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
    data->dispatch_wake.notify();
}
void filter_remove(void* ptr,obs_source_t*){
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
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
void* create(obs_data_t* settings,obs_source_t* source) {
    auto* audio=obs_get_audio();
    const uint32_t rate=audio?audio_output_get_sample_rate(audio):0;
    if(rate!=48000 && rate!=44100){
        blog(LOG_WARNING,"[Sound stems] Requires OBS stereo project at 44.1 or 48 kHz");
        return nullptr;
    }
    StemFilter* data=nullptr;
    try{data=new StemFilter(rate);}catch(...){return nullptr;}
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
        "void sound_stem_live_v1(out string status, out int windows, out int automatic_retries, out int delay_ms)",snapshot,data);
    proc_handler_add(obs_source_get_proc_handler(source),
        "void sound_stem_fanout_v1(out int packets, out int drops, out int subscribers)",fanout_snapshot,data);
    signal_handler_connect(obs_source_get_signal_handler(source),"enable",enabled_changed,data);
    blog(LOG_INFO,"[Sound stems] Experimental stereo delayed filter created (rate=%u, delay=12s)",rate);
    return data;
}
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
