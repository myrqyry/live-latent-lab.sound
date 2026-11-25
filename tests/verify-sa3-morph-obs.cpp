#include <obs.h>
#include <obs-module.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <string>
using namespace std::chrono_literals;
struct Capture{std::atomic<uint64_t> frames{0},nonzero{0},bad{0};};
static void capture(void* p,obs_source_t*,const audio_data* a,bool){
    auto& c=*static_cast<Capture*>(p);
    if(!a->data[0]||!a->data[1]){++c.bad;return;}
    for(int ch=0;ch<2;++ch){
        auto* f=reinterpret_cast<const float*>(a->data[ch]);
        for(unsigned i=0;i<a->frames;++i){if(!std::isfinite(f[i])||std::abs(f[i])>1.001f)++c.bad;if(std::abs(f[i])>.001f)++c.nonzero;}
    }
    c.frames+=a->frames;
}
static long long counter(obs_source_t* s,const char* key){
    calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v3",&d);
    auto v=ok?calldata_int(&d,key):-1;calldata_free(&d);return v;
}
static bool wait(obs_source_t* s,const char* key,long long n,int ms=15000){
    for(int i=0;i<ms/20;++i){if(counter(s,key)>=n)return true;std::this_thread::sleep_for(20ms);}return false;
}
static bool click(obs_source_t* s,const char* key){
    auto* p=obs_source_properties(s);auto* b=obs_properties_get(p,key);if(!b){auto* group=obs_properties_get(p,"latent_morph");if(group)b=obs_properties_get(obs_property_group_content(group),key);}if(b)obs_property_button_clicked(b,s);
    obs_properties_destroy(p);return b;
}
int main(int argc,char**argv){
    if(argc!=6||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_module_t* m=nullptr;if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 5;
    auto* d=obs_data_create();obs_data_set_string(d,"models_dir",argv[3]);obs_data_set_string(d,"morph_a",argv[4]);obs_data_set_string(d,"morph_b",argv[5]);obs_data_set_double(d,"morph_mix",.5);
    auto* s=obs_source_create("sound_audio_diffusion_source","Morph fixture",d,nullptr);
    if(!s)return 6;
    Capture c;obs_source_add_audio_capture_callback(s,capture,&c);obs_set_output_source(1,s);obs_source_set_audio_active(s,true);
    std::this_thread::sleep_for(350ms);
    bool available=click(s,"morph");
    bool generated=available&&wait(s,"played",1);
    bool audio=c.frames>=95000&&c.frames<=97000&&c.nonzero>1000&&!c.bad;
    auto first=c.frames.load();bool replay=generated&&click(s,"replay")&&wait(s,"played",2)&&c.frames-first>=95000&&c.frames-first<=97000;
    obs_data_set_string(d,"morph_b","/nonexistent/sound-morph.wav");obs_source_update(s,d);
    bool missing=available&&click(s,"morph")&&wait(s,"failed",1,5000)&&counter(s,"played")==2;
    obs_data_set_string(d,"morph_b",argv[5]);obs_data_set_bool(d,"morph_ramp",true);obs_data_set_double(d,"morph_mix",1);obs_source_update(s,d);
    bool recovered=available&&click(s,"morph")&&wait(s,"played",3)&&!c.bad;
    auto before=c.frames.load();
    obs_data_set_string(d,"device","cpu");obs_source_update(s,d);
    click(s,"morph");
    bool working=false;
    for(int i=0;i<200&&!working;++i){
        calldata_t status{};proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v3",&status);
        const char* stage=calldata_string(&status,"stage");
        working=stage&&std::string(stage)=="morphing";calldata_free(&status);
        if(!working)std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(25ms);click(s,"stop");std::this_thread::sleep_for(500ms);
    bool cancelled=working&&c.frames==before&&counter(s,"played")==3&&counter(s,"failed")==1;
    obs_data_set_string(d,"device","auto");obs_source_update(s,d);
    bool retry=click(s,"morph")&&wait(s,"played",4)&&!c.bad;
    printf("SA3_MORPH_CANCEL cancelled=%d retry=%d\n",cancelled,retry);
    bool ok=available&&generated&&audio&&replay&&missing&&recovered&&cancelled&&retry;
    printf("SA3_MORPH_OBS available=%d generated=%d audio=%d replay=%d missing=%d recovered=%d pass=%d\n",available,generated,audio,replay,missing,recovered,ok);
    obs_source_remove_audio_capture_callback(s,capture,&c);obs_set_output_source(1,nullptr);obs_source_release(s);obs_data_release(d);obs_wait_for_destroy_queue();obs_shutdown();return ok?0:1;
}
