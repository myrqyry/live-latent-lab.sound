#include <obs.h>
#include <obs-module.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <string>
#include <algorithm>
#include <cstdlib>
using namespace std::chrono_literals;
struct Capture {
    std::atomic<uint64_t> frames{0},nonzero{0},bad{0},range_bad{0},last_end{0},gap_ns{0};
    unsigned rate=48000;
};
static void capture(void* p,obs_source_t*,const audio_data* a,bool){
    auto& c=*static_cast<Capture*>(p);
    if(!a->data[0]||!a->data[1]){++c.bad;return;}
    for(unsigned ch=0;ch<2;ch++){
        auto* f=reinterpret_cast<const float*>(a->data[ch]);
        for(unsigned i=0;i<a->frames;i++){
            if(!std::isfinite(f[i]))++c.bad;
            if(std::abs(f[i])>1.001f)++c.range_bad;
            if(std::abs(f[i])>.001f)++c.nonzero;
        }
    }
    auto previous=c.last_end.exchange(a->timestamp+uint64_t(a->frames)*1000000000ULL/c.rate);
    if(previous&&a->timestamp>previous){
        auto gap=a->timestamp-previous,old=c.gap_ns.load();
        while(gap>old&&!c.gap_ns.compare_exchange_weak(old,gap)){}
    }
    if(previous&&a->timestamp+2000000<previous)++c.bad;
    c.frames+=a->frames;
}
static int64_t status(obs_source_t* s,const char* key){
    calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v1",&d);
    auto n=ok?calldata_int(&d,key):-1;calldata_free(&d);return n;
}
struct Session {
    bool called=false,continuous=false,finishing=false,generating=false,next_ready=false;
    int64_t generation_ms=0,buffer_ms=0,played_ms=0,underruns=0,next_ms=0;
};
static Session session(obs_source_t* s){
    calldata_t d{};Session a;
    a.called=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v3",&d);
    if(a.called){
        a.continuous=calldata_bool(&d,"continuous");a.finishing=calldata_bool(&d,"finishing");
        a.generating=calldata_bool(&d,"generating");a.next_ready=calldata_bool(&d,"next_ready");
        a.generation_ms=calldata_int(&d,"generation_ms");a.buffer_ms=calldata_int(&d,"buffer_ms");
        a.played_ms=calldata_int(&d,"played_ms");a.underruns=calldata_int(&d,"underruns");a.next_ms=calldata_int(&d,"next_ms");
    }
    calldata_free(&d);return a;
}
static bool wait_status(obs_source_t* s,const char* key,int64_t n,int ms=25000){
    for(int i=0;i<ms/20;i++){if(status(s,key)>=n)return true;if(status(s,"failed")>0)return false;std::this_thread::sleep_for(20ms);}return false;
}
static bool button(obs_source_t* s,const char* key){
    auto* p=obs_source_properties(s);auto* b=obs_properties_get(p,key);
    if(b)obs_property_button_clicked(b,s);
    obs_properties_destroy(p);return b!=nullptr;
}
int main(int argc,char** argv){
    if(argc!=4||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_audio_info ai{};ai.samples_per_sec=std::getenv("SOUND_DIFFUSION_TEST_44100")?44100:48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;
    v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;
    v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_module_t* m=nullptr;
    if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 5;
    const bool adaptive=std::getenv("SOUND_SA3_ADAPTIVE")!=nullptr;
    auto* cfg=obs_data_create();obs_data_set_bool(cfg,"adaptive_buffer",adaptive);obs_data_set_string(cfg,"models_dir",argv[3]);
    obs_data_set_string(cfg,"prompt","instrumental mellow electronic pop, toy keyboards, soft drums");
    obs_data_set_double(cfg,"duration",6);obs_data_set_int(cfg,"seed",42);
    auto* s=obs_source_create("sound_audio_diffusion_source","Continuous fixture",cfg,nullptr);
    if(!s)return 7;
    const bool telemetry=session(s).called;
    if(!telemetry){puts("SA3_SESSION_STATUS=0");obs_source_release(s);obs_data_release(cfg);obs_wait_for_destroy_queue();obs_shutdown();return 8;}
    Capture c;c.rate=ai.samples_per_sec;obs_source_add_audio_capture_callback(s,capture,&c);
    obs_set_output_source(1,s);obs_source_set_audio_mixers(s,1);obs_source_set_audio_active(s,true);
    std::this_thread::sleep_for(350ms);
    bool started=button(s,"continuous");
    if(!started){puts("SA3_CONTINUOUS_BUTTON=0");return 8;}
    if(adaptive&&std::getenv("SOUND_SA3_PREFILL_CANCEL")){
        bool in_prefill=false;
        for(int i=0;i<500&&!in_prefill;++i){
            calldata_t d{};
            const bool read=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_buffer_v1",&d);
            in_prefill=read&&calldata_bool(&d,"prefilling")&&session(s).generating&&c.frames==0;
            calldata_free(&d);
            if(!in_prefill)std::this_thread::sleep_for(20ms);
        }
        button(s,"stop");
        std::this_thread::sleep_for(500ms);
        auto stopped=session(s);
        calldata_t d{};
        const bool read=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_buffer_v1",&d);
        const bool clean=read&&!stopped.continuous&&!stopped.generating&&!stopped.next_ready&&!calldata_bool(&d,"adaptive")&&
            !calldata_bool(&d,"prefilling")&&calldata_int(&d,"queued_clips")==0&&c.frames==0;
        calldata_free(&d);
        obs_data_set_double(cfg,"duration",2);obs_source_update(s,cfg);
        bool recovered=button(s,"generate")&&wait_status(s,"played",1);
        bool ok=in_prefill&&clean&&recovered&&status(s,"failed")==0&&c.frames>=ai.samples_per_sec*2-1000&&c.frames<=ai.samples_per_sec*2+1000&&!c.bad&&!c.range_bad;
        printf("SA3_PREFILL_CANCEL in_prefill=%d clean=%d recovered=%d frames=%llu pass=%d\n",in_prefill,clean,recovered,(unsigned long long)c.frames.load(),ok);
        obs_source_remove_audio_capture_callback(s,capture,&c);obs_set_output_source(1,nullptr);obs_source_release(s);obs_data_release(cfg);obs_wait_for_destroy_queue();obs_shutdown();return ok?0:1;
    }
    bool reserve_ok=!adaptive;
    if(adaptive){
        calldata_t d{};
        const bool supported=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_buffer_v1",&d);
        calldata_free(&d);
        if(!supported){puts("SA3_ADAPTIVE_BUFFER=0");button(s,"stop");obs_source_remove_audio_capture_callback(s,capture,&c);obs_set_output_source(1,nullptr);obs_source_release(s);obs_data_release(cfg);obs_wait_for_destroy_queue();obs_shutdown();return 9;}
        for(int i=0;i<750;++i){
            d={};proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_buffer_v1",&d);
            const auto count=calldata_int(&d,"queued_clips"),target=calldata_int(&d,"target_clips");
            if(calldata_bool(&d,"adaptive")&&count<=3&&target>=2&&target<=3&&calldata_int(&d,"prefill_ms")>=12000)reserve_ok=true;
            calldata_free(&d);
            if(reserve_ok||status(s,"failed")>0)break;
            std::this_thread::sleep_for(20ms);
        }
    }
    bool first=wait_status(s,"completed",1);
    const bool compete=adaptive&&std::getenv("SOUND_SA3_COMPETE")!=nullptr;
    obs_source_t* competitor=nullptr;
    bool competing_started=!compete;
    if(compete){
        auto* settings=obs_data_create();obs_data_set_string(settings,"models_dir",argv[3]);
        obs_data_set_string(settings,"prompt","instrumental orchestral percussion");
        obs_data_set_double(settings,"duration",60);obs_data_set_int(settings,"steps",32);obs_data_set_int(settings,"seed",7);
        competitor=obs_source_create("sound_audio_diffusion_source","Competing diffusion fixture",settings,nullptr);
        obs_data_release(settings);
        if(competitor){
            obs_set_output_source(2,competitor);obs_source_set_audio_mixers(competitor,1);obs_source_set_audio_active(competitor,true);
            std::this_thread::sleep_for(350ms);
            competing_started=button(competitor,"generate")&&wait_status(competitor,"started",1,2000);
        }
    }
    obs_data_set_string(cfg,"prompt","mellow instrumental, gradually introduce piano and brushed drums");obs_source_update(s,cfg);
    bool lookahead=wait_status(s,"completed",2,10000);
    const bool soak=std::getenv("SOUND_SA3_SOAK")!=nullptr;
    const int segments=soak?10:3;
    bool ready=false,progressing=false,valid=true;
    long long max_reserve=0,max_target=0;
    int64_t previous=0;
    for(int i=0;i<(segments*7+15)*10&&status(s,"played")<segments;++i){
        auto a=session(s);
        if(adaptive){
            calldata_t d{};proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_buffer_v1",&d);
            max_reserve=std::max(max_reserve,calldata_int(&d,"reserve_ms"));
            max_target=std::max(max_target,calldata_int(&d,"target_clips"));
            valid=valid&&calldata_int(&d,"queued_clips")<=3;
            calldata_free(&d);
        }
        ready|=a.next_ready&&a.next_ms>0&&a.generation_ms>0;
        progressing|=a.generating&&a.continuous&&a.buffer_ms>0;
        valid=valid&&a.called&&a.continuous&&a.buffer_ms>=0&&a.buffer_ms<=57000&&a.played_ms>=previous&&a.underruns==0;
        previous=a.played_ms;
        if(status(s,"failed")>0)break;
        std::this_thread::sleep_for(100ms);
    }
    bool three=status(s,"played")>=segments;
    bool session_ok=ready&&(progressing||adaptive)&&valid;
    bool graceful=button(s,"finish_segment");
    for(int i=0;i<150&&session(s).continuous;++i)std::this_thread::sleep_for(100ms);
    auto done=session(s);
    graceful=graceful&&!done.continuous&&!done.generating&&!done.next_ready&&done.buffer_ms==0&&done.underruns==0&&done.played_ms>=segments*6000;
    printf("SA3_SESSION_STATUS ready=%d progressing=%d valid=%d graceful=%d segments=%d played_ms=%lld generation_ms=%lld underruns=%lld\n",
        ready,progressing,valid,graceful,segments,(long long)done.played_ms,(long long)done.generation_ms,(long long)done.underruns);
    auto frames=c.frames.load();
    auto gap=c.gap_ns.load();
    auto failed=status(s,"failed");
    button(s,"stop");std::this_thread::sleep_for(200ms);auto stopped=c.frames.load();
    std::this_thread::sleep_for(800ms);
    bool stop=c.frames==stopped;
    const bool competing_completed=!compete||(competitor&&status(competitor,"completed")>=1&&status(competitor,"failed")==0);
    if(competitor){
        button(competitor,"stop");obs_set_output_source(2,nullptr);obs_source_release(competitor);
        printf("SA3_BUFFER_COMPETITOR started=%d completed=%d\n",competing_started,competing_completed);
    }
    bool ok=reserve_ok&&competing_started&&competing_completed&&first&&lookahead&&three&&failed==0&&frames>=uint64_t(ai.samples_per_sec)*(segments*6-1)&&frames<=uint64_t(ai.samples_per_sec)*(segments*6+8)&&
            c.nonzero>ai.samples_per_sec*8&&!c.bad&&!c.range_bad&&gap<2000000&&stop&&session_ok&&graceful;
    if(adaptive)printf("SA3_ADAPTIVE_BUFFER prefilled=%d max_reserve_ms=%lld max_target=%lld pass=%d\n",reserve_ok,max_reserve,max_target,ok);
    printf("SA3_CONTINUOUS rate=%u first=%d lookahead=%d three=%d frames=%llu gap_ns=%llu bad=%llu range_bad=%llu failures=%lld stop=%d pass=%d\n",
      c.rate,first,lookahead,three,(unsigned long long)frames,(unsigned long long)gap,(unsigned long long)c.bad.load(),(unsigned long long)c.range_bad.load(),(long long)failed,stop,ok);
    obs_source_remove_audio_capture_callback(s,capture,&c);obs_set_output_source(1,nullptr);
    obs_source_release(s);obs_data_release(cfg);obs_wait_for_destroy_queue();obs_shutdown();return ok?0:1;
}
