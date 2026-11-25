#include <obs.h>
#include <obs-module.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <string>
#include <cstdlib>
using namespace std::chrono_literals;
struct Capture {std::atomic<uint64_t> frames{0},nonzero{0},bad{0},last{0};};
static void capture(void* p,obs_source_t*,const audio_data* a,bool){
    auto& c=*static_cast<Capture*>(p);
    if(!a->data[0]||!a->data[1]){++c.bad;return;}
    for(unsigned ch=0;ch<2;ch++){
        auto* f=reinterpret_cast<const float*>(a->data[ch]);
        for(unsigned i=0;i<a->frames;i++){if(!std::isfinite(f[i]))++c.bad;if(std::abs(f[i])>.001f)++c.nonzero;}
    }
    if(c.last.exchange(a->timestamp)>a->timestamp)++c.bad;
    c.frames+=a->frames;
}
static int64_t status(obs_source_t* s,const char* key){
    calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v1",&d);
    auto n=ok?calldata_int(&d,key):-1;calldata_free(&d);return n;
}
static bool wait_status(obs_source_t* s,const char* key,int64_t n,int ms=15000){
    for(int i=0;i<ms/20;i++){if(status(s,key)>=n)return true;std::this_thread::sleep_for(20ms);}return false;
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
    auto* p=obs_get_source_properties("sound_audio_diffusion_source");
    if(!p){puts("DIFFUSION_SOURCE_REGISTERED=0");obs_shutdown();return 6;}
    bool advanced=obs_properties_get(p,"cfg_scale")&&obs_properties_get(p,"distribution_shift")&&obs_properties_get(p,"generation_tail")&&obs_properties_get(p,"continuation_tail");
    obs_properties_destroy(p);
    if(!advanced){puts("DIFFUSION_MODEL_CONTROLS=0");return 6;}
    auto* cfg=obs_data_create();obs_data_set_string(cfg,"models_dir",argv[3]);
    obs_data_set_string(cfg,"prompt","gentle toy keyboard, bass, soft drums, instrumental bedroom pop");
    obs_data_set_double(cfg,"duration",2.0);obs_data_set_int(cfg,"seed",42);
    obs_data_set_double(cfg,"cfg_scale",1.5);obs_data_set_int(cfg,"distribution_shift",3);obs_data_set_double(cfg,"generation_tail",3);obs_data_set_double(cfg,"continuation_tail",4);
    auto* s=obs_source_create("sound_audio_diffusion_source","Diffusion fixture",cfg,nullptr);
    if(!s)return 7;
    Capture c;obs_source_add_audio_capture_callback(s,capture,&c);
    obs_set_output_source(1,s);obs_source_set_audio_mixers(s,1);obs_source_set_audio_active(s,true);
    std::this_thread::sleep_for(350ms);
    bool generated=button(s,"generate")&&wait_status(s,"completed",1)&&wait_status(s,"played",1);
    auto first=c.frames.load();
    bool audio=first>=ai.samples_per_sec*2-1000&&first<=ai.samples_per_sec*2+1000&&c.nonzero>1000&&!c.bad;
    bool replay=button(s,"replay")&&wait_status(s,"played",2)&&status(s,"completed")==1&&c.frames>=first+ai.samples_per_sec*2-1000;
    obs_data_set_double(cfg,"duration",60);obs_data_set_int(cfg,"steps",32);obs_source_update(s,cfg);
    bool cancel=button(s,"generate")&&wait_status(s,"started",2)&&wait_status(s,"progress",2)&&button(s,"stop");
    std::this_thread::sleep_for(1500ms);
    cancel=cancel&&status(s,"completed")==1&&status(s,"played")==2;
    obs_data_set_string(cfg,"models_dir","/nonexistent/sound-models");obs_data_set_double(cfg,"duration",2);obs_data_set_int(cfg,"steps",8);
    obs_source_update(s,cfg);
    bool missing=button(s,"generate")&&wait_status(s,"failed",1);
    obs_data_set_string(cfg,"models_dir",argv[3]);obs_source_update(s,cfg);
    bool recovery=button(s,"generate")&&wait_status(s,"completed",2)&&wait_status(s,"played",3);
    obs_source_set_enabled(s,false);auto before=status(s,"started");button(s,"generate");
    std::this_thread::sleep_for(250ms);bool disabled=status(s,"started")==before;
    printf("DIFFUSION_DISABLED=%d\n",disabled);
    obs_source_remove_audio_capture_callback(s,capture,&c);obs_set_output_source(1,nullptr);
    obs_source_release(s);obs_data_release(cfg);obs_wait_for_destroy_queue();obs_shutdown();
    puts("DIFFUSION_MODEL_CONTROLS=1");
    printf("DIFFUSION_OBS generated=%d audio=%d frames=%llu replay=%d cancel=%d missing=%d recovery=%d bad=%llu\n",
        generated,audio,(unsigned long long)first,replay,cancel,missing,recovery,(unsigned long long)c.bad.load());
    return generated&&audio&&replay&&cancel&&missing&&recovery&&disabled&&!c.bad?0:1;
}
