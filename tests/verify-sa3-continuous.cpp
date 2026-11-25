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
    auto* cfg=obs_data_create();obs_data_set_string(cfg,"models_dir",argv[3]);
    obs_data_set_string(cfg,"prompt","instrumental mellow electronic pop, toy keyboards, soft drums");
    obs_data_set_double(cfg,"duration",6);obs_data_set_int(cfg,"seed",42);
    auto* s=obs_source_create("sound_audio_diffusion_source","Continuous fixture",cfg,nullptr);
    if(!s)return 7;
    Capture c;c.rate=ai.samples_per_sec;obs_source_add_audio_capture_callback(s,capture,&c);
    obs_set_output_source(1,s);obs_source_set_audio_mixers(s,1);obs_source_set_audio_active(s,true);
    std::this_thread::sleep_for(350ms);
    bool started=button(s,"continuous");
    if(!started){puts("SA3_CONTINUOUS_BUTTON=0");return 8;}
    bool first=wait_status(s,"completed",1);
    obs_data_set_string(cfg,"prompt","mellow instrumental, gradually introduce piano and brushed drums");obs_source_update(s,cfg);
    bool lookahead=wait_status(s,"completed",2,10000);
    bool three=wait_status(s,"played",3,26000);
    auto frames=c.frames.load();
    auto gap=c.gap_ns.load();
    auto failed=status(s,"failed");
    button(s,"stop");std::this_thread::sleep_for(200ms);auto stopped=c.frames.load();
    std::this_thread::sleep_for(800ms);
    bool stop=c.frames==stopped;
    bool ok=first&&lookahead&&three&&failed==0&&frames>=ai.samples_per_sec*17&&frames<=ai.samples_per_sec*20&&
            c.nonzero>ai.samples_per_sec*8&&!c.bad&&!c.range_bad&&gap<2000000&&stop;
    printf("SA3_CONTINUOUS rate=%u first=%d lookahead=%d three=%d frames=%llu gap_ns=%llu bad=%llu range_bad=%llu failures=%lld stop=%d pass=%d\n",
      c.rate,first,lookahead,three,(unsigned long long)frames,(unsigned long long)gap,(unsigned long long)c.bad.load(),(unsigned long long)c.range_bad.load(),(long long)failed,stop,ok);
    obs_source_remove_audio_capture_callback(s,capture,&c);obs_set_output_source(1,nullptr);
    obs_source_release(s);obs_data_release(cfg);obs_wait_for_destroy_queue();obs_shutdown();return ok?0:1;
}
