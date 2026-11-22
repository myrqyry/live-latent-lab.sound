#include <obs.h>
#include <obs-module.h>
#include <callback/proc.h>
#include <util/platform.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <mutex>
using namespace std::chrono_literals;
static std::mutex ui_mutex;
static std::vector<std::pair<obs_task_t,void*>> ui_tasks;
static void queue_ui(obs_task_t task,void* ptr,bool wait){
    if(wait){std::abort();}
    std::lock_guard lock(ui_mutex);ui_tasks.emplace_back(task,ptr);
}
static void drain_ui(){
    std::vector<std::pair<obs_task_t,void*>> tasks;
    {std::lock_guard lock(ui_mutex);tasks.swap(ui_tasks);}
    for(auto& t:tasks)t.first(t.second);
}

static const char* name(void*){return "STT audio fixture";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info input={.id="sound_stt_fixture",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=create,.destroy=destroy};
struct Result{std::string status,text;int64_t sequence=0,dropped=0,ms=0;bool available=false;};
static Result read(obs_source_t* f){
    Result r;calldata_t d{};
    if(proc_handler_call(obs_source_get_proc_handler(f),"sound_stt_read_v1",&d)){
        r.status=calldata_string(&d,"status")?calldata_string(&d,"status"):"";
        r.text=calldata_string(&d,"text")?calldata_string(&d,"text"):"";
        r.sequence=calldata_int(&d,"sequence");r.dropped=calldata_int(&d,"dropped_samples");
        r.ms=calldata_int(&d,"inference_ms");r.available=calldata_bool(&d,"available");
    }calldata_free(&d);return r;
}
struct Dry {std::atomic<uint64_t> frames{0};std::atomic<bool> mismatch{false};std::atomic<double> power{0};};
static void capture(void* ptr,obs_source_t*,const audio_data* a,bool){
    auto* d=static_cast<Dry*>(ptr);if(!a->data[0]||!a->data[1])return;
    auto* l=reinterpret_cast<const float*>(a->data[0]);auto* r=reinterpret_cast<const float*>(a->data[1]);
    double p=0;for(size_t i=0;i<a->frames;++i){p+=l[i]*l[i];if(std::abs(r[i]-.5f*l[i])>1e-6)d->mismatch=true;}
    double old=d->power.load();while(!d->power.compare_exchange_weak(old,old+p)){}
    d->frames+=a->frames;
}
static void feed(obs_source_t* s,const std::vector<float>& data,uint32_t rate,obs_source_t* f,std::string& transcript,int64_t& seq){
    const size_t block=rate/100;std::vector<float> l(block),r(block);
    uint64_t start=os_gettime_ns();
    const auto clock=std::chrono::steady_clock::now();
    for(size_t pos=0;pos<data.size();pos+=block){
        for(size_t i=0;i<block;++i){l[i]=pos+i<data.size()?data[pos+i]:0;r[i]=l[i]*.5f;}
        obs_source_audio a{};a.data[0]=reinterpret_cast<uint8_t*>(l.data());a.data[1]=reinterpret_cast<uint8_t*>(r.data());
        a.frames=block;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;a.samples_per_sec=rate;a.timestamp=start+pos*1000000000ULL/rate;
        obs_source_output_audio(s,&a);std::this_thread::sleep_until(clock+std::chrono::nanoseconds((pos+block)*1000000000ULL/rate));drain_ui();
        auto t=read(f);if(t.available&&t.sequence!=seq){transcript+=" "+t.text;seq=t.sequence;}
    }
}
int main(int argc,char** argv){
    const bool parakeet=argc==8;
    if((argc!=6&&!parakeet)||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_set_ui_task_handler(queue_ui);drain_ui();
    const uint32_t rate=std::stoul(argv[5]);
    obs_audio_info ai{};ai.samples_per_sec=rate;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_module_t* m=nullptr;if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 4;
    auto* props=obs_get_source_properties("sound_stt_filter");
    if(!props){puts("SOUND_STT_REGISTRY=0");obs_shutdown();return 1;}
    bool ui=obs_properties_get(props,"model_path")&&obs_properties_get(props,"caption_target");
    if(parakeet){
        auto* selector=obs_properties_get(props,"backend");
        auto* d=obs_data_create();obs_data_set_string(d,"backend","parakeet");
        if(selector)obs_property_modified(selector,d);
        auto* path=obs_properties_get(props,"parakeet_model_path");
        auto* lib=obs_properties_get(props,"parakeet_library");
        ui=ui&&selector&&path&&lib&&obs_property_visible(path)&&obs_property_visible(lib)&&
          !obs_property_visible(obs_properties_get(props,"language"))&&!obs_property_visible(obs_properties_get(props,"model_path"));
        if(selector) {
            obs_data_set_string(d,"backend","whisper");obs_property_modified(selector,d);
            ui=ui&&path&&!obs_property_visible(path)&&obs_property_visible(obs_properties_get(props,"language"));
        }
        obs_data_release(d);
        printf("SOUND_STT_PARAKEET_PICKER=%d\n",ui);
        if(!ui){obs_properties_destroy(props);obs_shutdown();return 1;}
    }
    obs_properties_destroy(props);
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 5;
    obs_module_t* text=nullptr;
    if(obs_open_module(&text,"/usr/local/lib/obs-plugins/text-freetype2.so","/usr/local/share/obs/obs-plugins/text-freetype2")!=MODULE_SUCCESS||!obs_init_module(text))return 6;
    auto* target=obs_source_create("text_ft2_source_v2","STT test captions",nullptr,nullptr);
    if(!target)target=obs_source_create("text_ft2_source","STT test captions",nullptr,nullptr);
    obs_register_source(&input);
    auto* s=obs_source_create("sound_stt_fixture","STT input",nullptr,nullptr);
    if(!target||!s)return 7;
    auto* cfg=obs_data_create();obs_data_set_string(cfg,"model_path",argv[3]);
    if(parakeet){
        obs_data_set_string(cfg,"backend","parakeet");
        obs_data_set_string(cfg,"parakeet_model_path",argv[3]);obs_data_set_string(cfg,"parakeet_library",argv[6]);
        obs_data_set_string(cfg,"model_path",argv[7]);
    }
    obs_data_set_string(cfg,"caption_target",obs_source_get_uuid(target));obs_data_set_int(cfg,"max_phrase_ms",12000);
    obs_data_set_int(cfg,"silence_ms",900);obs_data_set_int(cfg,"caption_hold_ms",15000);
    auto* f=obs_source_create("sound_stt_filter","STT",cfg,nullptr);obs_data_release(cfg);
    if(!f)return 8;
    obs_source_filter_add(s,f);obs_set_output_source(1,s);obs_source_set_audio_mixers(s,1);
    obs_source_inc_active(s);obs_source_set_audio_active(s,true);
    Dry dry;obs_source_add_audio_capture_callback(s,capture,&dry);
    for(int i=0;i<500&&read(f).status!="ready";++i)std::this_thread::sleep_for(20ms);
    bool ready=read(f).status=="ready";
    std::string transcript;int64_t seq=0;
    feed(s,std::vector<float>(rate,0),rate,f,transcript,seq);
    bool silence=seq==0&&transcript.empty();
    std::ifstream file(argv[4],std::ios::binary|std::ios::ate);
    const auto bytes=file.tellg();if(bytes<=0)return 9;
    std::vector<float> audio(size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(audio.data()),bytes);
    auto stream=audio;stream.resize(stream.size()+rate*2,0);
    feed(s,stream,rate,f,transcript,seq);
    for(int i=0;i<180;++i){
        drain_ui();auto r=read(f);if(r.available&&r.sequence!=seq){transcript+=" "+r.text;seq=r.sequence;}
        if(seq&&r.status=="ready"&&i>20)break;
        std::this_thread::sleep_for(50ms);
    }
    drain_ui();auto result=read(f);
    std::string lower=transcript;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
    bool words=lower.find("country")!=std::string::npos&&lower.find("ask")!=std::string::npos;
    cfg=obs_source_get_settings(target);std::string caption=obs_data_get_string(cfg,"text");obs_data_release(cfg);
    bool routed=!caption.empty()&&caption==result.text;
    // Live tuning must not invalidate the recognizer or its final transcript.
    cfg=obs_data_create();
    obs_data_set_int(cfg,"voice_threshold_db",-43);
    obs_data_set_int(cfg,"silence_ms",700);
    obs_data_set_int(cfg,"caption_hold_ms",14000);
    obs_data_set_string(cfg,"initial_prompt","country");
    obs_source_update(f,cfg);obs_data_release(cfg);
    const auto after_hot=read(f);
    const bool hot_stable=result.available&&after_hot.available&&after_hot.status=="ready"&&
        after_hot.sequence==result.sequence&&after_hot.text==result.text;
    printf("SOUND_STT_HOT_UPDATE_STABLE=%d\n",hot_stable);
    bool clean=!dry.mismatch&&dry.frames>audio.size()&&dry.power>1&&result.dropped==0;
    // Feed a speech-only fragment and then stop sending frames entirely.
    // The worker must wake on its silence deadline to finalize it.
    const auto flush_before=seq;
    const auto fragment_size=std::min(audio.size(),size_t(rate)*2);
    auto fragment=std::vector<float>(audio.begin(),audio.begin()+fragment_size);
    std::string idle_text;
    feed(s,fragment,rate,f,idle_text,seq);
    for(int i=0;i<120&&seq==flush_before;++i){
        drain_ui();
        auto r=read(f);
        if(r.sequence>seq){idle_text=r.text;seq=r.sequence;}
        std::this_thread::sleep_for(40ms);
    }
    const bool stopped_source_flush=seq>flush_before&&!idle_text.empty();
    printf("SOUND_STT_STOPPED_SOURCE_FLUSH=%d\n",stopped_source_flush);
    obs_source_set_muted(s,true);
    const auto before=seq;feed(s,audio,rate,f,transcript,seq);
    bool muted=seq==before;
    obs_source_set_enabled(f,false);
    for(int i=0;i<200&&read(f).status!="off";++i){drain_ui();std::this_thread::sleep_for(20ms);}
    drain_ui();
    bool off=read(f).status=="off"&&!read(f).available;
    cfg=obs_source_get_settings(target);off=off&&std::string(obs_data_get_string(cfg,"text")).empty();obs_data_release(cfg);
    cfg=obs_data_create();obs_data_set_string(cfg,parakeet?"parakeet_model_path":"model_path","/nonexistent/stt-model.bin");obs_source_update(f,cfg);obs_data_release(cfg);
    obs_source_set_enabled(f,true);
    for(int i=0;i<200&&read(f).status!="model_error";++i){drain_ui();std::this_thread::sleep_for(20ms);}
    bool missing=read(f).status=="model_error"&&!read(f).available;
    cfg=obs_data_create();obs_data_set_string(cfg,parakeet?"parakeet_model_path":"model_path",argv[3]);obs_source_update(f,cfg);obs_data_release(cfg);
    for(int i=0;i<500&&read(f).status!="ready";++i){drain_ui();std::this_thread::sleep_for(20ms);}
    bool recovered=read(f).status=="ready";
    printf("SOUND_STT_LIFECYCLE off=%d missing=%d recovered=%d\n",off,missing,recovered);
    bool ok=ui&&ready&&silence&&words&&routed&&hot_stable&&stopped_source_flush&&clean&&muted&&off&&missing&&recovered;
    if(parakeet){
        // Same audio source and filter; preserve the independently saved
        // model paths across both switches and actually run Whisper again.
        cfg=obs_data_create();obs_data_set_string(cfg,"backend","whisper");obs_source_update(f,cfg);obs_data_release(cfg);
        bool invalidated=!read(f).available;
        std::this_thread::sleep_for(100ms);
        for(int i=0;i<500&&read(f).status!="ready";++i){drain_ui();std::this_thread::sleep_for(20ms);}
        obs_source_set_muted(s,false);
        std::string switched;auto next=seq;
        feed(s,stream,rate,f,switched,next);
        for(int i=0;i<180&&next==seq;++i){drain_ui();auto r=read(f);if(r.available&&r.sequence!=next){switched+=" "+r.text;next=r.sequence;}std::this_thread::sleep_for(50ms);}
        std::transform(switched.begin(),switched.end(),switched.begin(),[](unsigned char c){return std::tolower(c);});
        bool whisper=next>seq&&switched.find("country")!=std::string::npos;
        cfg=obs_source_get_settings(f);
        bool preserved=std::string(obs_data_get_string(cfg,"parakeet_model_path"))==argv[3]&&std::string(obs_data_get_string(cfg,"model_path"))==argv[7];
        obs_data_set_string(cfg,"backend","parakeet");obs_source_update(f,cfg);obs_data_release(cfg);
        invalidated=invalidated&&!read(f).available;
        std::this_thread::sleep_for(200ms);
        for(int i=0;i<500&&read(f).status!="ready";++i){drain_ui();std::this_thread::sleep_for(20ms);}
        std::string again;auto last=next;feed(s,stream,rate,f,again,last);
        for(int i=0;i<180&&last==next;++i){drain_ui();auto r=read(f);if(r.available&&r.sequence!=last){again+=" "+r.text;last=r.sequence;}std::this_thread::sleep_for(50ms);}
        std::transform(again.begin(),again.end(),again.begin(),[](unsigned char c){return std::tolower(c);});
        bool returned=last>next&&again.find("country")!=std::string::npos;
        printf("SOUND_STT_PARAKEET_SWITCH invalidated=%d whisper=%d saved=%d returned=%d\n",invalidated,whisper,preserved,returned);
        ok=ok&&invalidated&&whisper&&preserved&&returned;
    }

    printf("SOUND_STT_REAL rate=%u ui=%d ready=%d silence=%d words=%d routed=%d dry=%d muted=%d sequences=%lld inference_ms=%lld frames=%llu pass=%d\n",rate,ui,ready,silence,words,routed,clean,muted,(long long)seq,(long long)result.ms,(unsigned long long)dry.frames.load(),ok);
    printf("SOUND_STT_FIXTURE_TEXT: %s\n",transcript.c_str());
    obs_source_remove_audio_capture_callback(s,capture,&dry);obs_source_dec_active(s);obs_set_output_source(1,nullptr);
    obs_source_filter_remove(s,f);obs_source_release(f);obs_source_release(s);obs_wait_for_destroy_queue();drain_ui();obs_source_release(target);obs_wait_for_destroy_queue();drain_ui();obs_shutdown();
    return ok?0:1;
}
