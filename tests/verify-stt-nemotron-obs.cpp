#include "sa3-stt-coexist.hpp"
#include <obs.h>
#include <obs-module.h>
#include <callback/proc.h>
#include <util/platform.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
static int partials=0;static int64_t partial_rev=0;static size_t first_partial=0;
static bool partial_routed=false,early_final=false;
static obs_source_t* caption_source=nullptr;
static void read_partial(obs_source_t* f,size_t pos){
    calldata_t d{};
    if(proc_handler_call(obs_source_get_proc_handler(f),"sound_stt_read_v2",&d)&&calldata_bool(&d,"available")&&!calldata_bool(&d,"final")){
        auto rev=calldata_int(&d,"revision");auto* text=calldata_string(&d,"text");
        if(rev!=partial_rev&&text&&*text){partial_rev=rev;++partials;if(!first_partial){first_partial=pos;early_final=read(f).available;}
            auto* cfg=obs_source_get_settings(caption_source);
            partial_routed|=std::string(obs_data_get_string(cfg,"text"))==text;
            obs_data_release(cfg);
        }
    }
    calldata_free(&d);
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
        read_partial(f,pos+block);auto t=read(f);if(t.available&&t.sequence!=seq){transcript+=" "+t.text;seq=t.sequence;}
    }
}
int main(int argc,char** argv){
    if((argc!=7&&argc!=8)||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_set_ui_task_handler(queue_ui);drain_ui();
    const uint32_t rate=std::stoul(argv[5]);
    obs_audio_info ai{};ai.samples_per_sec=rate;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_module_t* m=nullptr;if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 4;
    auto* props=obs_get_source_properties("sound_stt_filter");
    if(!props){puts("SOUND_STT_REGISTRY=0");obs_shutdown();return 1;}
    bool ui=obs_properties_get(props,"model_path")&&obs_properties_get(props,"caption_target");
    auto* backend=obs_properties_get(props,"backend");
    auto* d=obs_data_create();obs_data_set_string(d,"backend","nemotron");
    if(backend)obs_property_modified(backend,d);
    auto* model=obs_properties_get(props,"nemotron_model_path");
    ui=ui&&model&&obs_property_visible(model)&&obs_properties_get(props,"nemotron_right_context")&&
       !obs_property_visible(obs_properties_get(props,"model_path"));
    obs_data_release(d);obs_properties_destroy(props);
    printf("SOUND_STT_NEMOTRON_PICKER=%d\n",ui);
    if(!ui){obs_shutdown();return 1;}
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 5;
    obs_module_t* text=nullptr;
    if(obs_open_module(&text,"/usr/local/lib/obs-plugins/text-freetype2.so","/usr/local/share/obs/obs-plugins/text-freetype2")!=MODULE_SUCCESS||!obs_init_module(text))return 6;
    auto* target=obs_source_create("text_ft2_source_v2","STT test captions",nullptr,nullptr);
    if(!target)target=obs_source_create("text_ft2_source","STT test captions",nullptr,nullptr);
    caption_source=target;obs_register_source(&input);
    auto* s=obs_source_create("sound_stt_fixture","STT input",nullptr,nullptr);
    if(!target||!s)return 7;
    auto* cfg=obs_data_create();obs_data_set_string(cfg,"backend","nemotron");
    obs_data_set_string(cfg,"nemotron_model_path",argv[3]);
    if(const char* device=std::getenv("SOUND_TEST_NEMOTRON_DEVICE"))
        obs_data_set_string(cfg,"nemotron_device",device);
    if(argc==8)obs_data_set_string(cfg,"model_path",argv[7]);
    if(std::string(argv[6])!="-")obs_data_set_string(cfg,"nemotron_library",argv[6]);
    obs_data_set_string(cfg,"caption_target",obs_source_get_uuid(target));obs_data_set_int(cfg,"max_phrase_ms",15000);
    obs_data_set_int(cfg,"silence_ms",1500);obs_data_set_int(cfg,"caption_hold_ms",15000);
    auto* f=obs_source_create("sound_stt_filter","STT",cfg,nullptr);obs_data_release(cfg);
    if(!f)return 8;
    obs_source_filter_add(s,f);obs_set_output_source(1,s);obs_source_set_audio_mixers(s,1);
    obs_source_inc_active(s);obs_source_set_audio_active(s,true);
    Dry dry;obs_source_add_audio_capture_callback(s,capture,&dry);
    const auto init_started=std::chrono::steady_clock::now();
    // CUDA context creation and cold model loads can exceed the 10 s
    // performance target. Measure that target separately from functional
    // readiness, instead of feeding audio while status is still "loading".
    constexpr int functional_deadline_ms=20000;
    for(int i=0;i<functional_deadline_ms/20&&read(f).status!="ready";++i)
        std::this_thread::sleep_for(20ms);
    const auto initial=read(f);
    const auto init_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-init_started).count();
    bool ready=initial.status=="ready";
    const bool within_startup_target=ready&&init_ms<=10000;
    printf("SOUND_STT_INITIAL_READY backend=nemotron status=%s elapsed_ms=%lld deadline_ms=%d within_10s=%d ready=%d\n",
        initial.status.c_str(),(long long)init_ms,functional_deadline_ms,within_startup_target,ready);
    std::string transcript;int64_t seq=0;
    feed(s,std::vector<float>(rate,0),rate,f,transcript,seq);
    bool silence=seq==0&&transcript.empty()&&partials==0;
    std::ifstream file(argv[4],std::ios::binary|std::ios::ate);
    const auto bytes=file.tellg();if(bytes<=0)return 9;
    std::vector<float> audio(size_t(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(audio.data()),bytes);
    auto stream=audio;stream.resize(stream.size()+rate*2,0);
    Sa3SttCoexist diffusion;
    const bool diffusion_started=diffusion.start();
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
    calldata_t final_view{};
    bool final_ok=proc_handler_call(obs_source_get_proc_handler(f),"sound_stt_read_v2",&final_view)
        &&calldata_bool(&final_view,"available")&&calldata_bool(&final_view,"final")
        &&calldata_string(&final_view,"text")&&result.text==calldata_string(&final_view,"text");
    calldata_free(&final_view);
    bool routed=!caption.empty()&&caption==result.text&&final_ok;
    bool live=partials>=2&&first_partial<audio.size()&&partial_routed&&!early_final;
    printf("SOUND_STT_NEMOTRON_LIVE rate=%u updates=%d first_audio_ms=%zu routed=%d pass=%d\n",rate,partials,first_partial*1000/rate,partial_routed,live);
    bool clean=!dry.mismatch&&dry.frames>audio.size()&&dry.power>1&&result.dropped==0;
    obs_source_set_muted(s,true);
    const auto before=seq;const auto before_partials=partials;feed(s,audio,rate,f,transcript,seq);
    bool muted=seq==before&&partials==before_partials;
    obs_source_set_enabled(f,false);
    for(int i=0;i<200&&read(f).status!="off";++i){drain_ui();std::this_thread::sleep_for(20ms);}
    drain_ui();
    bool off=read(f).status=="off"&&!read(f).available;
    cfg=obs_source_get_settings(target);off=off&&std::string(obs_data_get_string(cfg,"text")).empty();obs_data_release(cfg);
    cfg=obs_data_create();obs_data_set_string(cfg,"nemotron_model_path","/nonexistent/stt-model.bin");obs_source_update(f,cfg);obs_data_release(cfg);
    obs_source_set_enabled(f,true);
    for(int i=0;i<200&&read(f).status!="model_error";++i){drain_ui();std::this_thread::sleep_for(20ms);}
    bool missing=read(f).status=="model_error"&&!read(f).available;
    cfg=obs_data_create();obs_data_set_string(cfg,"nemotron_model_path",argv[3]);obs_source_update(f,cfg);obs_data_release(cfg);
    for(int i=0;i<500&&read(f).status!="ready";++i){drain_ui();std::this_thread::sleep_for(20ms);}
    bool recovered=read(f).status=="ready";
    printf("SOUND_STT_LIFECYCLE off=%d missing=%d recovered=%d\n",off,missing,recovered);
    bool ok=live&&ui&&ready&&silence&&words&&routed&&clean&&muted&&off&&missing&&recovered;
    if(argc==8&&rate==48000){
        cfg=obs_data_create();obs_data_set_string(cfg,"backend","whisper");obs_source_update(f,cfg);obs_data_release(cfg);
        std::this_thread::sleep_for(200ms);
        for(int i=0;i<500&&read(f).status!="ready";++i){drain_ui();std::this_thread::sleep_for(20ms);}
        obs_source_set_muted(s,false);
        std::string switched;auto next=seq;
        feed(s,stream,rate,f,switched,next);
        for(int i=0;i<180&&next==seq;++i){drain_ui();auto r=read(f);if(r.available&&r.sequence!=next){switched+=" "+r.text;next=r.sequence;}std::this_thread::sleep_for(50ms);}
        std::transform(switched.begin(),switched.end(),switched.begin(),[](unsigned char c){return std::tolower(c);});
        bool whisper=next>seq&&switched.find("country")!=std::string::npos;
        cfg=obs_source_get_settings(f);
        bool saved=std::string(obs_data_get_string(cfg,"nemotron_model_path"))==argv[3]&&
            std::string(obs_data_get_string(cfg,"model_path"))==argv[7];
        obs_data_set_string(cfg,"backend","nemotron");obs_source_update(f,cfg);obs_data_release(cfg);
        std::this_thread::sleep_for(200ms);
        for(int i=0;i<500&&read(f).status!="ready";++i){drain_ui();std::this_thread::sleep_for(20ms);}
        auto old_partials=partials;
        std::vector<float> excerpt(audio.begin(),audio.begin()+std::min<size_t>(audio.size(),rate*4));
        std::string unused;auto last=next;
        feed(s,excerpt,rate,f,unused,last);
        bool returned=partials>old_partials;
        obs_source_set_enabled(f,false);
        for(int i=0;i<200&&read(f).status!="off";++i){drain_ui();std::this_thread::sleep_for(20ms);}
        drain_ui();
        calldata_t gone{};bool invalidated=proc_handler_call(obs_source_get_proc_handler(f),"sound_stt_read_v2",&gone)&&
             !calldata_bool(&gone,"available")&&!read(f).available;calldata_free(&gone);
        cfg=obs_source_get_settings(target);
        invalidated=invalidated&&std::string(obs_data_get_string(cfg,"text")).empty();obs_data_release(cfg);
        printf("SOUND_STT_NEMOTRON_SWITCH whisper=%d saved=%d returned=%d invalidated=%d\n",whisper,saved,returned,invalidated);
        ok=ok&&whisper&&saved&&returned&&invalidated;
    }

    const bool diffusion_finished=diffusion.finish();
    ok=ok&&diffusion_started&&diffusion_finished;
    printf("SOUND_STT_REAL rate=%u ui=%d ready=%d silence=%d words=%d routed=%d dry=%d muted=%d sequences=%lld inference_ms=%lld frames=%llu pass=%d\n",rate,ui,ready,silence,words,routed,clean,muted,(long long)seq,(long long)result.ms,(unsigned long long)dry.frames.load(),ok);
    printf("SOUND_STT_FIXTURE_TEXT: %s\n",transcript.c_str());
    obs_source_remove_audio_capture_callback(s,capture,&dry);obs_source_dec_active(s);obs_set_output_source(1,nullptr);
    obs_source_filter_remove(s,f);obs_source_release(f);obs_source_release(s);obs_wait_for_destroy_queue();drain_ui();obs_source_release(target);obs_wait_for_destroy_queue();drain_ui();obs_shutdown();
    return ok?0:1;
}
