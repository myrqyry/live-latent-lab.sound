// LocalVocal-inspired local captions, with Sound's bounded audio-tap design.
// No upstream code copied. Audio is always returned unchanged.
#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <media-io/audio-io.h>
#include <callback/proc.h>
#include <util/platform.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "engines/stt_whisper.hpp"
#include "engines/stt_parakeet.hpp"
#include "engines/stt_nemotron.hpp"
#include "framework/stt_capture.hpp"
#include "interop/sound_signal_client.hpp"
namespace {
using namespace std::chrono_literals;
constexpr int hz=16000,frame_size=320;
struct Config {
    std::string backend="whisper",model,parakeet_model,parakeet_library,target,language="en",prompt;
    std::string nemotron_model,nemotron_library,nemotron_language="en-US";
    int nemotron_threads=4,nemotron_right=3;
    int threads=4,threshold=-45,silence=600,max_phrase=8000,hold=5000;
    bool translate=false,while_muted=false;
};
struct CaptionBridge {
    std::mutex mutex;
    std::string desired_target,desired_text,owned_target,owned_text;
    bool queued=false,closed=false;
};
struct State {
    obs_source_t* context;
    const unsigned rate,channels;
    stts::SttCapture capture;
    std::atomic<bool> stopping{false},accept{false},gap{false},while_muted{false},enabled{false},attached{false};
    std::atomic<uint64_t> epoch{1},revision{1},dropped{0};
    std::mutex control;
    Config cfg;
    std::mutex result;
    std::string status="loading",text,partial_text;
    uint64_t partial_epoch=0,partial_ms=0,result_revision=0;
    uint64_t sequence=0,inference_ms=0,publication_epoch=0;
    std::chrono::steady_clock::time_point observed{},partial_observed{};
    std::shared_ptr<CaptionBridge> captions=std::make_shared<CaptionBridge>();
    std::thread worker;
    State(obs_source_t* c,unsigned r,unsigned n):context(c),rate(r),channels(n){}
};
bool is_text(obs_source_t* source){
    const char* raw=source?obs_source_get_unversioned_id(source):nullptr;
    return raw&&(std::string(raw)=="text_ft2_source"||std::string(raw)=="text_gdiplus");
}
Config config(obs_data_t* d){
    Config c;
    c.backend=obs_data_get_string(d,"backend");
    c.nemotron_model=obs_data_get_string(d,"nemotron_model_path");
    c.nemotron_library=obs_data_get_string(d,"nemotron_library");
    c.nemotron_language=obs_data_get_string(d,"nemotron_language");
    c.parakeet_model=obs_data_get_string(d,"parakeet_model_path");
    c.parakeet_library=obs_data_get_string(d,"parakeet_library");
    c.model=obs_data_get_string(d,"model_path");c.target=obs_data_get_string(d,"caption_target");
    c.language=obs_data_get_string(d,"language");c.prompt=obs_data_get_string(d,"initial_prompt");
    if(c.prompt.size()>1024)c.prompt.clear();
    auto get=[&](const char* k,int a,int b){return int(std::clamp<int64_t>(obs_data_get_int(d,k),a,b));};
    c.nemotron_threads=get("nemotron_threads",1,12);
    c.nemotron_right=int(obs_data_get_int(d,"nemotron_right_context"));
    c.threads=get("threads",1,16);c.threshold=get("voice_threshold_db",-65,-20);
    c.silence=get("silence_ms",200,2000);c.max_phrase=get("max_phrase_ms",1500,15000);
    c.hold=get("caption_hold_ms",1000,30000);
    c.translate=obs_data_get_bool(d,"translate");c.while_muted=obs_data_get_bool(d,"process_muted");
    return c;
}
void caption(State&,const std::string&,const std::string&,uint64_t,bool);
void update(void* ptr,obs_data_t* d){
    auto* s=static_cast<State*>(ptr);
    auto c=config(d);
    s->accept=false;
    {std::lock_guard lock(s->control);
    s->cfg=std::move(c);s->while_muted=s->cfg.while_muted;
    ++s->epoch;++s->revision;}
    caption(*s,"","",0,false);
}
void set_status(State& s,const char* status){
    std::lock_guard lock(s.result);
    s.status=status;s.text.clear();s.publication_epoch=0;s.partial_text.clear();s.partial_epoch=0;
}
// Target is explicitly user-selected, stable by UUID, with a type and
// ownership check when clearing. No UI/property refresh from audio thread.
bool write_caption(const std::string& target,const std::string& text,const std::string* expected=nullptr){
    if(target.empty())return false;
    auto* source=obs_get_source_by_uuid(target.c_str());
    if(!source)return false;
    bool ok=false;
    if(is_text(source)){
        auto* settings=obs_source_get_settings(source);
        if(!obs_data_get_bool(settings,"from_file")&&
           (!expected||*expected==obs_data_get_string(settings,"text"))){
            auto* change=obs_data_create();obs_data_set_string(change,"text",text.c_str());
            obs_source_update(source,change);obs_data_release(change);ok=true;
        }
        obs_data_release(settings);
    }
    obs_source_release(source);return ok;
}
// This task changes text settings only. It never rebuilds properties,
// drives a property callback, or retains a raw filter pointer.
void caption_task(void* ptr){
    std::unique_ptr<std::shared_ptr<CaptionBridge>> task(static_cast<std::shared_ptr<CaptionBridge>*>(ptr));
    auto& b=**task;
    std::string target,text,old_target,old_text;
    {
        std::lock_guard lock(b.mutex);
        target=b.closed?"":b.desired_target;
        text=b.closed?"":b.desired_text;
        old_target=b.owned_target;old_text=b.owned_text;
        b.queued=false;
    }
    if(!old_text.empty()&&(target!=old_target||text.empty()))
        write_caption(old_target,"",&old_text);
    bool sent=!target.empty()&&!text.empty()&&write_caption(target,text);
    {
        std::lock_guard lock(b.mutex);
        b.owned_target=sent?target:"";b.owned_text=sent?text:"";
    }
}
void caption(State& s,const std::string& target,const std::string& text,uint64_t epoch=0,bool close=false){
    auto b=s.captions;
    bool enqueue=false;
    {
        std::lock_guard lock(b->mutex);
        if(epoch&&s.epoch!=epoch)return;
        if(b->closed)return;
        b->closed=close;
        b->desired_target=target;b->desired_text=text;
        if(!b->queued){b->queued=true;enqueue=true;}
    }
    if(enqueue)obs_queue_task(OBS_TASK_UI,caption_task,new std::shared_ptr<CaptionBridge>(std::move(b)),false);
}
void attached(void* ptr,obs_source_t*){
    auto& s=*static_cast<State*>(ptr);
    s.accept=false;s.enabled=obs_source_enabled(s.context);s.attached=true;++s.epoch;
}
void detached(void* ptr,obs_source_t*){
    auto& s=*static_cast<State*>(ptr);
    s.accept=false;s.attached=false;++s.epoch;caption(s,"","");
}
void enabled_changed(void* ptr,calldata_t* d){
    auto& s=*static_cast<State*>(ptr);
    s.accept=false;s.enabled=calldata_bool(d,"enabled");++s.epoch;
    if(!s.enabled)caption(s,"","");
}
struct Abort {State* s;uint64_t epoch;};
bool aborted(void* p){
    auto& a=*static_cast<Abort*>(p);
    return a.s->stopping.load()||a.s->epoch.load()!=a.epoch;
}
void run(State& s){
    const auto runtime=(std::filesystem::path(obs_get_module_binary_path(obs_current_module())).parent_path()/
                        "stt-runtime/libwhisper.so.1").string();
    stts::SttWhisper engine;
    stts::SttParakeet parakeet;
    stts::SttNemotron nemotron;
    const auto default_nemotron=(std::filesystem::path(runtime).parent_path()/"libsound-nemotron-runtime.so").string();
    bool streaming=false;
    size_t fed=0;
    std::unique_ptr<audio_resampler_t,decltype(&audio_resampler_destroy)> resampler(nullptr,audio_resampler_destroy);
    Config c;
    uint64_t rev=0,stream_epoch=0,expected_ts=0;
    bool ready=false,enabled=false;
    bool written=false;
    std::vector<float> phrase;
    std::array<float,frame_size> frame{};
    size_t used=0;
    std::deque<float> preroll;
    int speech_frames=0,quiet_frames=0;
    auto last_packet=std::chrono::steady_clock::now();
    auto clear_caption=[&]{
        if(written)caption(s,"","");
        written=false;
    };
    auto reset_stream=[&]{
        nemotron.reset();streaming=false;fed=0;
        std::lock_guard lock(s.result);s.partial_text.clear();s.partial_epoch=0;
    };
    auto reset=[&]{
        reset_stream();
        phrase.clear();preroll.clear();used=0;speech_frames=quiet_frames=0;expected_ts=0;
        const resample_info input={s.rate,AUDIO_FORMAT_FLOAT_PLANAR,SPEAKERS_MONO},
            output={hz,AUDIO_FORMAT_FLOAT_PLANAR,SPEAKERS_MONO};
        resampler.reset(audio_resampler_create(&output,&input));
    };
    // Advance the persistent Nemotron stream with only previously unsent
    // audio. v1 remains a completed-phrase contract; v2 exposes revisions.
    auto advance_stream=[&](uint64_t e,bool finalize,std::string& t)->bool{
        if(s.epoch!=e||s.stopping)return false;
        if(!streaming){if(!nemotron.begin())return false;streaming=true;fed=0;}
        constexpr size_t chunk=8960;
        while(phrase.size()-fed>=chunk||(finalize&&fed<phrase.size())){
            if(s.epoch!=e||s.stopping)return false;
            const auto count=std::min(chunk,phrase.size()-fed);
            if(!nemotron.feed(phrase.data()+fed,count,t))return false;
            fed+=count;
        }
        return !finalize||nemotron.finish(t);
    };
    auto publish_partial=[&](uint64_t e){
        if(phrase.size()-fed<8960||speech_frames<10)return;
        const auto begin=std::chrono::steady_clock::now();
        std::string t;
        if(!advance_stream(e,false,t)){
            if(s.epoch==e&&!s.stopping){clear_caption();reset_stream();set_status(s,"inference_error");ready=false;s.accept=false;}
            return;
        }
        if(t.empty()||s.epoch!=e||s.stopping)return;
        bool changed=false;
        {
            std::lock_guard lock(s.result);
            if(s.epoch!=e)return;
            changed=s.partial_epoch!=e||s.partial_text!=t;
            if(changed){
                s.partial_text=t;s.partial_epoch=e;++s.result_revision;
            }
            s.partial_observed=std::chrono::steady_clock::now();
            s.partial_ms=std::chrono::duration_cast<std::chrono::milliseconds>(s.partial_observed-begin).count();
        }
        if(changed&&!c.target.empty()){caption(s,c.target,t,e);written=true;}
    };
    auto publish=[&](uint64_t e){
        if(speech_frames<10||phrase.empty()||s.epoch!=e){phrase.clear();speech_frames=quiet_frames=0;reset_stream();return;}
        if(c.backend!="nemotron")while(phrase.size()<size_t(hz))phrase.push_back(0.f);
        Abort a{&s,e};
        const auto begin=std::chrono::steady_clock::now();
        std::string t;
        const bool ok=c.backend=="nemotron"?advance_stream(e,true,t):c.backend=="parakeet"?parakeet.transcribe(phrase,t):
            engine.transcribe(phrase,c.threads,c.language,c.translate,c.prompt,aborted,&a,t);
        phrase.clear();speech_frames=quiet_frames=0;
        bool had_partial=false;
        {std::lock_guard lock(s.result);had_partial=!s.partial_text.empty();}
        nemotron.reset();streaming=false;fed=0;
        if(aborted(&a))return;
        if(!ok){clear_caption();set_status(s,"inference_error");return;}
        if(t.empty()){if(had_partial)clear_caption();reset_stream();return;}
        if(!c.target.empty()){caption(s,c.target,t,e);written=true;}
        std::lock_guard lock(s.result);
        if(s.epoch!=e)return;
        s.partial_text.clear();s.partial_epoch=0;
        s.status="ready";s.text=std::move(t);++s.sequence;++s.result_revision;
        s.publication_epoch=e;s.observed=std::chrono::steady_clock::now();
        s.inference_ms=std::chrono::duration_cast<std::chrono::milliseconds>(s.observed-begin).count();
    };
    while(!s.stopping){
        bool on=s.enabled.load()&&s.attached.load();
        auto requested=s.revision.load();
        if(requested!=rev||on!=enabled){
            s.accept=false;clear_caption();engine.close();parakeet.close();nemotron.close();ready=false;reset();
            {
                std::lock_guard lock(s.control);
                c=s.cfg;rev=s.revision.load();
            }
            enabled=on;++s.epoch;stream_epoch=s.epoch.load();
            set_status(s,on?"loading":"off");
            if(on){
                ready=resampler&&(c.backend=="whisper"?engine.open(runtime,c.model,c.language,c.translate):
                    c.backend=="parakeet"?parakeet.open(c.parakeet_library,c.parakeet_model):
                    c.backend=="nemotron"?nemotron.open(c.nemotron_library.empty()?default_nemotron:c.nemotron_library,c.nemotron_model,c.nemotron_threads,c.nemotron_language,c.nemotron_right)&&nemotron.begin():false);
                if(c.backend=="nemotron"){
                    if(ready)nemotron.reset();else nemotron.close();
                }
                set_status(s,ready?"ready":"model_error");
                if(ready)blog(LOG_INFO,"[Sound/STT] SOUND_STT_NATIVE_20261118_S813 CPU %s model ready at %u Hz",c.backend.c_str(),s.rate);
                else blog(LOG_WARNING,"[Sound/STT] Could not load local model/runtime or language unsupported");
            }
            s.accept=ready&&!s.stopping&&s.revision==rev&&s.enabled&&s.attached;
        }
        auto e=s.epoch.load();
        if(stream_epoch!=e){
            stream_epoch=e;clear_caption();reset();set_status(s,ready?"ready":enabled?"model_error":"off");
        }
        if(written){
            bool expired;
            {std::lock_guard lock(s.result);expired=std::chrono::steady_clock::now()-(s.partial_text.empty()?s.observed:s.partial_observed)>std::chrono::milliseconds(c.hold);}
            if(expired)clear_caption();
        }
        stts::SttBlock b;
        if(!s.capture.pop(b)){
            // Flush an ended media source even if it stops emitting frames.
            if(ready&&!phrase.empty()&&std::chrono::steady_clock::now()-last_packet>std::chrono::milliseconds(c.silence))
                publish(stream_epoch);
            std::this_thread::sleep_for(4ms);continue;
        }
        if(!ready||b.epoch!=stream_epoch||s.epoch!=stream_epoch||s.revision!=rev)continue;
        if(expected_ts&&(b.timestamp>expected_ts+100000000ULL||b.timestamp+100000000ULL<expected_ts)){
            clear_caption();reset();set_status(s,"ready");
        }
        expected_ts=b.timestamp+uint64_t(b.count)*1000000000ULL/s.rate;
        last_packet=std::chrono::steady_clock::now();
        if(!resampler){s.accept=false;ready=false;set_status(s,"resampler_error");continue;}
        const uint8_t* input[MAX_AUDIO_CHANNELS]{};uint8_t* output[MAX_AUDIO_CHANNELS]{};
        input[0]=reinterpret_cast<const uint8_t*>(b.samples.data());
        uint32_t count=0;uint64_t offset=0;
        if(!audio_resampler_resample(resampler.get(),output,&count,&offset,input,b.count)||(!output[0]&&count)){
            s.accept=false;ready=false;set_status(s,"resampler_error");continue;
        }
        const auto* pcm=reinterpret_cast<const float*>(output[0]);
        const float limit=std::pow(10.f,c.threshold/20.f);
        for(uint32_t i=0;i<count&&ready&&s.epoch==stream_epoch&&!s.stopping;++i){
            frame[used++]=pcm[i];if(used!=frame.size())continue;used=0;
            double power=0;for(auto sample:frame)power+=sample*sample;
            const bool voice=power/frame.size()>=limit*limit;
            if(phrase.empty()&&!voice){
                for(float sample:frame)preroll.push_back(sample);
                while(preroll.size()>hz/5)preroll.pop_front();
                continue;
            }
            if(phrase.empty()){
                phrase.assign(preroll.begin(),preroll.end());preroll.clear();
            }
            phrase.insert(phrase.end(),frame.begin(),frame.end());
            if(voice){++speech_frames;quiet_frames=0;}else ++quiet_frames;
            if(quiet_frames*20>=c.silence||phrase.size()>=size_t(c.max_phrase)*hz/1000)
                publish(stream_epoch);
            else if(c.backend=="nemotron")publish_partial(stream_epoch);
        }
    }
    s.accept=false;caption(s,"","",0,true);
}
void entry(State* s){try{run(*s);}catch(...){s->accept=false;set_status(*s,"worker_error");caption(*s,"","",0,true);}}
void snapshot(void* ptr,calldata_t* d){
    auto* s=static_cast<State*>(ptr);std::lock_guard lock(s->result);
    int hold;
    {std::lock_guard cfg_lock(s->control);hold=s->cfg.hold;}
    const bool fresh=s->publication_epoch==s->epoch&&s->sequence&&s->status=="ready"&&s->enabled&&s->attached&&
         std::chrono::steady_clock::now()-s->observed<=std::chrono::milliseconds(hold);
    calldata_set_string(d,"status",s->status.c_str());calldata_set_string(d,"text",fresh?s->text.c_str():"");
    calldata_set_bool(d,"available",fresh&&!s->text.empty());
    calldata_set_int(d,"sequence",s->sequence);calldata_set_int(d,"inference_ms",s->inference_ms);
    calldata_set_int(d,"dropped_samples",s->dropped.load());
}
void snapshot_v2(void* ptr,calldata_t* d){
    auto* s=static_cast<State*>(ptr);std::lock_guard lock(s->result);
    int hold;
    {std::lock_guard cfg_lock(s->control);hold=s->cfg.hold;}
    const bool current=s->status=="ready"&&s->enabled&&s->attached;
    const auto now=std::chrono::steady_clock::now();
    const bool partial=current&&s->partial_epoch==s->epoch&&!s->partial_text.empty()&&
        now-s->partial_observed<=std::chrono::milliseconds(hold);
    const bool final=current&&s->publication_epoch==s->epoch&&s->sequence&&!s->text.empty()&&
        now-s->observed<=std::chrono::milliseconds(hold);
    calldata_set_string(d,"status",s->status.c_str());
    calldata_set_string(d,"text",partial?s->partial_text.c_str():final?s->text.c_str():"");
    calldata_set_bool(d,"available",partial||final);calldata_set_bool(d,"final",!partial&&final);
    calldata_set_int(d,"sequence",s->sequence);calldata_set_int(d,"revision",s->result_revision);
    calldata_set_int(d,"inference_ms",partial?s->partial_ms:s->inference_ms);
    calldata_set_int(d,"dropped_samples",s->dropped.load());
}
obs_audio_data* audio(void* ptr,obs_audio_data* packet){
    auto* s=static_cast<State*>(ptr);
    if(!packet||!packet->frames||!s->accept)return packet;
    auto* parent=obs_filter_get_parent(s->context);
    if(!s->while_muted&&parent&&obs_source_muted(parent)){
        if(!s->gap.exchange(true))++s->epoch;
        return packet;
    }
    s->gap=false;
    auto e=s->epoch.load();
    std::array<const float*,MAX_AUDIO_CHANNELS> planes{};
    for(size_t ch=0;ch<s->channels;++ch){
        if(!packet->data[ch]){++s->epoch;return packet;}
        planes[ch]=reinterpret_cast<float*>(packet->data[ch]);
    }
    stts::SttBlock b;b.epoch=e;
    for(uint32_t offset=0;offset<packet->frames;){
        if(!s->accept||s->epoch!=e)break;
        b.count=std::min<uint32_t>(b.samples.size(),packet->frames-offset);
        b.timestamp=packet->timestamp+uint64_t(offset)*1000000000ULL/s->rate;
        for(uint32_t i=0;i<b.count;++i){
            float sum=0;for(size_t ch=0;ch<s->channels;++ch)sum+=planes[ch][offset+i];
            b.samples[i]=std::isfinite(sum)?std::clamp(sum/float(s->channels),-1.f,1.f):0.f;
        }
        if(!s->capture.push(b)){s->dropped+=packet->frames-offset;++s->epoch;break;}
        offset+=b.count;
    }
    return packet;
}
void defaults(obs_data_t* d){
    obs_data_set_default_string(d,"backend","whisper");
    obs_data_set_default_string(d,"parakeet_model_path","");
    obs_data_set_default_string(d,"nemotron_model_path","");
    obs_data_set_default_string(d,"nemotron_library","");
    obs_data_set_default_string(d,"nemotron_language","en-US");
    obs_data_set_default_int(d,"nemotron_threads",4);
    obs_data_set_default_int(d,"nemotron_right_context",3);
    // Reuse the runtime already installed by Sight, without copying a
    // second MediaPipe library or embedding an absolute workstation path.
    const auto module=std::filesystem::path(obs_get_module_binary_path(obs_current_module()));
    const auto sibling=module.parent_path().parent_path().parent_path().parent_path()/
        "live-latent-lab-sight/bin/64bit/libmediapipe-1.2.3.so";
    std::error_code ec;
    obs_data_set_default_string(d,"parakeet_library",std::filesystem::is_regular_file(sibling,ec)?sibling.c_str():"");

    obs_data_set_default_string(d,"model_path","");
    obs_data_set_default_string(d,"caption_target","");
    obs_data_set_default_string(d,"language","en");
    obs_data_set_default_string(d,"initial_prompt","");
    obs_data_set_default_int(d,"threads",4);
    obs_data_set_default_int(d,"voice_threshold_db",-45);
    obs_data_set_default_int(d,"silence_ms",600);
    obs_data_set_default_int(d,"max_phrase_ms",8000);
    obs_data_set_default_int(d,"caption_hold_ms",5000);
    obs_data_set_default_bool(d,"translate",false);obs_data_set_default_bool(d,"process_muted",false);
}
bool add_target(void* p,obs_source_t* source){
    if(is_text(source))obs_property_list_add_string(static_cast<obs_property_t*>(p),obs_source_get_name(source),obs_source_get_uuid(source));
    return true;
}
void backend_visibility(obs_properties_t* p,const std::string& backend){
    for(auto key:{"nemotron_model_path","nemotron_library","nemotron_language","nemotron_threads","nemotron_right_context","nemotron_note"})
        obs_property_set_visible(obs_properties_get(p,key),backend=="nemotron");
    for(auto key:{"model_path","language","translate","threads","initial_prompt"})
        obs_property_set_visible(obs_properties_get(p,key),backend=="whisper");
    for(auto key:{"parakeet_model_path","parakeet_library","parakeet_note"})
        obs_property_set_visible(obs_properties_get(p,key),backend=="parakeet");
}
bool backend_changed(obs_properties_t* p,obs_property_t*,obs_data_t* d){
    backend_visibility(p,obs_data_get_string(d,"backend"));return true;
}
obs_properties_t* properties(void* ptr){
    auto* p=obs_properties_create();
    auto* backend=obs_properties_add_list(p,"backend","Speech recognition engine",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(backend,"Whisper (CPU)","whisper");
    obs_property_list_add_string(backend,"Parakeet TDT (MediaPipe CPU)","parakeet");
    obs_property_list_add_string(backend,"Nemotron 3.5 ASR (streaming CPU)","nemotron");
    obs_property_set_modified_callback(backend,backend_changed);
    obs_properties_add_path(p,"nemotron_model_path","Nemotron GGUF model",OBS_PATH_FILE,"GGUF models (*.gguf)",nullptr);
    obs_properties_add_path(p,"nemotron_library","Native Nemotron runtime (optional override)",OBS_PATH_FILE,"Shared libraries (*.so)",nullptr);
    obs_properties_add_text(p,"nemotron_language","Language locale (en-US, es-ES, auto, ...)",OBS_TEXT_DEFAULT);
    obs_properties_add_int(p,"nemotron_threads","Nemotron CPU threads",1,12,1);
    auto* context=obs_properties_add_list(p,"nemotron_right_context","Encoder right context",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(context,"0 frames (lowest lookahead)",0);
    obs_property_list_add_int(context,"3 frames (240 ms)",3);
    obs_property_list_add_int(context,"6 frames (480 ms)",6);
    obs_property_list_add_int(context,"13 frames (1040 ms)",13);
    obs_properties_add_text(p,"nemotron_note","Native streaming hypotheses; current phrase replaces its previous text. Actual latency also includes audio chunking and inference. Finalize on silence or maximum phrase.",OBS_TEXT_INFO);
    obs_properties_add_path(p,"parakeet_model_path","Parakeet model",OBS_PATH_FILE,"LiteRT-LM model (*.litertlm)",nullptr);
    obs_properties_add_path(p,"parakeet_library","MediaPipe Tasks 1.2.3 native library",OBS_PATH_FILE,"Shared library (*.so)",nullptr);
    obs_properties_add_text(p,"parakeet_note","Automatic language detection. CPU inference, completed phrases; no language override, translation or vocabulary prompt for this backend.",OBS_TEXT_INFO);
    obs_properties_add_path(p,"model_path","Whisper GGML model",OBS_PATH_FILE,"Whisper models (*.bin)",nullptr);
    auto* target=obs_properties_add_list(p,"caption_target","Caption text source",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(target,"None (read-only transcript signal)","");obs_enum_sources(add_target,target);
    std::string selected="whisper";
    if(ptr){
        auto* d=obs_source_get_settings(static_cast<State*>(ptr)->context);
        selected=obs_data_get_string(d,"backend");
        std::string saved=obs_data_get_string(d,"caption_target");obs_data_release(d);
        bool found=false;for(size_t i=0;i<obs_property_list_item_count(target);++i)found|=saved==obs_property_list_item_string(target,i);
        if(!saved.empty()&&!found){auto n=obs_property_list_add_string(target,"Saved target (unavailable)",saved.c_str());obs_property_list_item_disable(target,n,true);}
    }
    obs_properties_add_text(p,"language","Language code (en, auto, es, ...)",OBS_TEXT_DEFAULT);
    obs_properties_add_bool(p,"translate","Translate to English (multilingual model)");
    obs_properties_add_int(p,"threads","CPU threads",1,16,1);
    obs_properties_add_text(p,"initial_prompt","Vocabulary prompt (optional)",OBS_TEXT_DEFAULT);
    obs_properties_add_int_slider(p,"voice_threshold_db","Energy gate (dBFS)",-65,-20,1);
    obs_properties_add_int(p,"silence_ms","End-of-phrase silence (ms)",200,2000,20);
    obs_properties_add_int(p,"max_phrase_ms","Maximum phrase (ms)",1500,15000,100);
    obs_properties_add_int(p,"caption_hold_ms","Caption hold (ms)",1000,30000,100);
    obs_properties_add_bool(p,"process_muted","Transcribe muted audio");
    obs_properties_add_text(p,"note","Local CPU inference. Nemotron supports partial captions; other engines publish completed phrases. Audio is unchanged. Choose a dedicated OBS text source (not file-backed). Change settings to reload; disable filter to unload.",OBS_TEXT_INFO);
    backend_visibility(p,selected);
    return p;
}
const char* name(void*){return "Live Latent: STT";}
void* create(obs_data_t* d,obs_source_t* source){
    auto* a=obs_get_audio();if(!a)return nullptr;
    auto rate=audio_output_get_sample_rate(a);auto channels=audio_output_get_channels(a);
    if((rate!=48000&&rate!=44100)||!channels||channels>MAX_AUDIO_CHANNELS)return nullptr;
    try{
        auto s=std::make_unique<State>(source,rate,channels);s->cfg=config(d);s->while_muted=s->cfg.while_muted;s->enabled=obs_source_enabled(source);
        signal_handler_connect(obs_source_get_signal_handler(source),"enable",enabled_changed,s.get());
        try{s->worker=std::thread(entry,s.get());}
        catch(...){signal_handler_disconnect(obs_source_get_signal_handler(source),"enable",enabled_changed,s.get());throw;}
        proc_handler_add(obs_source_get_proc_handler(source),"void sound_stt_read_v1(out string text, out string status, out bool available, out int sequence, out int inference_ms, out int dropped_samples)",snapshot,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),"void sound_stt_read_v2(out string text, out string status, out bool available, out bool final, out int sequence, out int revision, out int inference_ms, out int dropped_samples)",snapshot_v2,s.get());
        sound_signals::register_broker(source);
        return s.release();
    }catch(...){return nullptr;}
}
void destroy(void* ptr){auto* s=static_cast<State*>(ptr);signal_handler_disconnect(obs_source_get_signal_handler(s->context),"enable",enabled_changed,s);s->stopping=true;s->accept=false;++s->epoch;if(s->worker.joinable())s->worker.join();delete s;}
}
extern "C" {
struct obs_source_info sound_stt_filter={
    .id="sound_stt_filter",.type=OBS_SOURCE_TYPE_FILTER,.output_flags=OBS_SOURCE_AUDIO,
    .get_name=name,.create=create,.destroy=destroy,.get_defaults=defaults,.get_properties=properties,.update=update,.filter_audio=audio,.filter_remove=detached,.filter_add=attached,
};
}
