#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <string>
#include <vector>
using namespace std::chrono_literals;
struct Capture {std::atomic<uint64_t> frames{0},nonzero{0},bad{0};std::atomic<float> peak{0};};
static void capture(void* p,obs_source_t*,const audio_data* a,bool){
    auto& c=*static_cast<Capture*>(p);
    if(!a->data[0]||!a->data[1]){++c.bad;return;}
    for(unsigned ch=0;ch<2;ch++){
        auto* f=reinterpret_cast<const float*>(a->data[ch]);
        for(unsigned i=0;i<a->frames;i++){if(!std::isfinite(f[i])||std::abs(f[i])>1.001f)++c.bad;float v=std::abs(f[i]),old=c.peak.load();while(v>old&&!c.peak.compare_exchange_weak(old,v)){}if(std::abs(f[i])>.001f)++c.nonzero;}
    }
    c.frames+=a->frames;
}
static int64_t status(obs_source_t* s,const char* key){
    calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v1",&d);
    auto n=ok?calldata_int(&d,key):-1;calldata_free(&d);return n;
}
static bool wait_status(obs_source_t* s,const char* key,int64_t n,int ms=20000){
    for(int i=0;i<ms/20;i++){if(status(s,key)>=n)return true;std::this_thread::sleep_for(20ms);}return false;
}
static bool button(obs_source_t* s,const char* key){
    auto* p=obs_source_properties(s);auto* b=obs_properties_get(p,key);
    if(b)obs_property_button_clicked(b,s);
    obs_properties_destroy(p);return b!=nullptr;
}
int main(int argc,char** argv){
    if(argc!=6||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;
    v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;
    v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_module_t* m=nullptr;
    if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 5;
    // Real libobs input source; the fixture emits a deterministic stereo tone.
    obs_source_info fixture{};fixture.id="sound_sa3_input_fixture";fixture.type=OBS_SOURCE_TYPE_INPUT;fixture.output_flags=OBS_SOURCE_AUDIO;
    fixture.get_name=[](void*)->const char*{return "SA3 test audio";};
    fixture.create=[](obs_data_t*,obs_source_t* s)->void*{return s;};fixture.destroy=[](void*){};
    obs_register_source(&fixture);
    auto* input=obs_source_create("sound_sa3_input_fixture","Capture fixture",nullptr,nullptr);
    obs_set_output_source(2,input);obs_source_set_audio_active(input,true);
    // Exact Sight wire-contract fixture; this is not neural camera inference.
    struct Motion {uint32_t abi_version,struct_size,source_width,source_height;uint64_t sequence;float energy;uint32_t reserved;};
    std::atomic<bool> fresh{true};
    obs_source_info video{};video.id="sound_sa3_video_fixture";video.type=OBS_SOURCE_TYPE_INPUT;video.output_flags=OBS_SOURCE_VIDEO;
    video.get_name=[](void*)->const char*{return "Sight fixture";};
    video.create=[](obs_data_t*,obs_source_t* s)->void*{return s;};video.destroy=[](void*){};
    video.get_width=[](void*)->uint32_t{return 320;};video.get_height=[](void*)->uint32_t{return 240;};
    obs_register_source(&video);
    obs_source_info motion{};motion.id="sight_u2net_filter";motion.type=OBS_SOURCE_TYPE_FILTER;motion.output_flags=OBS_SOURCE_VIDEO;
    motion.get_name=[](void*)->const char*{return "Motion fixture";};
    motion.create=[](obs_data_t*,obs_source_t* s)->void*{return s;};motion.destroy=[](void*){};
    motion.get_width=video.get_width;motion.get_height=video.get_height;
    obs_register_source(&motion);
    auto* video_source=obs_source_create(video.id,"Video fixture",nullptr,nullptr);
    auto* motion_source=obs_source_create_private(motion.id,"Motion fixture",nullptr);
    proc_handler_add(obs_source_get_proc_handler(motion_source),"void sight_motion_read_v1(in int max_age_ms, in ptr snapshot, out bool available)",
        [](void* p,calldata_t* d){
            void* raw=nullptr;calldata_set_bool(d,"available",false);
            if(!*static_cast<std::atomic<bool>*>(p)||!calldata_get_ptr(d,"snapshot",&raw)||!raw)return;
            auto& m=*static_cast<Motion*>(raw);
            if(m.abi_version!=1||m.struct_size!=sizeof(Motion))return;
            m.source_width=320;m.source_height=240;m.sequence=1;m.energy=.15f;m.reserved=0;calldata_set_bool(d,"available",true);
        },&fresh);
    obs_source_filter_add(video_source,motion_source);
    const auto sight_key=std::string(obs_source_get_uuid(video_source))+"/"+obs_source_get_uuid(motion_source);
    auto* cfg=obs_data_create();obs_data_set_string(cfg,"models_dir",argv[3]);
    obs_data_set_string(cfg,"prompt","a series of delicate glass chimes and ceramic clicks");
    obs_data_set_string(cfg,"variant","small-sfx");obs_data_set_double(cfg,"duration",2);obs_data_set_int(cfg,"seed",42);
    auto* s=obs_source_create("sound_audio_diffusion_source","Operations fixture",cfg,nullptr);
    if(!s)return 7;
    Capture c;obs_source_add_audio_capture_callback(s,capture,&c);
    obs_set_output_source(1,s);obs_source_set_audio_mixers(s,1);obs_source_set_audio_active(s,true);
    std::this_thread::sleep_for(350ms);
    int count=0;
    auto run=[&](const char* name,int seconds){
        auto start=c.frames.load(),nz=c.nonzero.load(),bad_before=c.bad.load();c.peak=0;
        bool ok=button(s,name)&&wait_status(s,"played",++count);
        auto frames=c.frames-start;
        bool pcm=frames>=uint64_t(seconds*48000-1000)&&frames<=uint64_t(seconds*48000+1000)&&c.nonzero-nz>1000&&c.bad==bad_before;
        printf("SA3_OPERATION name=%s frames=%llu nonzero=%llu peak=%.5f bad=%llu played=%lld pass=%d\n",name,(unsigned long long)frames,(unsigned long long)(c.nonzero-nz),c.peak.load(),(unsigned long long)(c.bad-bad_before),(long long)status(s,"played"),ok&&pcm);
        return ok&&pcm;
    };
    bool sfx=run("generate",2);
    obs_data_set_string(cfg,"variant","small-music");obs_data_set_string(cfg,"input_file",argv[4]);
    obs_data_set_string(cfg,"prompt","warm mellow piano and soft bass");obs_data_set_double(cfg,"transform_noise",.6);obs_source_update(s,cfg);
    bool transform=run("transform",2);
    obs_data_set_string(cfg,"input_file","");obs_data_set_double(cfg,"duration",2);obs_source_update(s,cfg);
    bool extend=run("extend",4);
    obs_data_set_string(cfg,"input_file","/nonexistent/sound-fixture.wav");obs_source_update(s,cfg);
    bool bad=button(s,"transform")&&wait_status(s,"failed",1)&&status(s,"played")==3;
    obs_data_set_string(cfg,"input_file","");obs_data_set_double(cfg,"duration",60);obs_source_update(s,cfg);
    bool capped=button(s,"extend")&&wait_status(s,"failed",2)&&status(s,"played")==3;
    // Real capture followed by native transform of 48 kHz audio.
    obs_data_set_double(cfg,"duration",2);obs_data_set_string(cfg,"input_source",obs_source_get_uuid(input));
    obs_data_set_double(cfg,"capture_seconds",2);obs_source_update(s,cfg);
    auto frame_start=c.frames.load();
    bool capture_started=button(s,"transform");
    std::thread feeder([&]{
        std::array<float,960> l{},r{};
        auto start=std::chrono::steady_clock::now();auto base=os_gettime_ns();
        for(unsigned n=0;n<140;n++){
            for(unsigned i=0;i<960;i++){
                l[i]=.2f*std::sin(float(n*960+i)*.0575959f);
                r[i]=.15f*std::sin(float(n*960+i)*.0712f);
            }
            obs_source_audio a{};a.data[0]=reinterpret_cast<const uint8_t*>(l.data());a.data[1]=reinterpret_cast<const uint8_t*>(r.data());
            a.frames=960;a.speakers=SPEAKERS_STEREO;a.samples_per_sec=48000;a.format=AUDIO_FORMAT_FLOAT_PLANAR;
            a.timestamp=base+uint64_t(n)*20000000ULL;
            obs_source_output_audio(input,&a);
            std::this_thread::sleep_until(start+std::chrono::milliseconds((n+1)*20));
        }
    });
    bool capture_play=capture_started&&wait_status(s,"played",++count);
    feeder.join();
    calldata_t v2{};bool snapshot=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v2",&v2);
    auto captured=calldata_int(&v2,"captured");calldata_free(&v2);
    bool captured_ok=snapshot&&captured==1&&capture_play&&c.frames-frame_start>=95000&&c.frames-frame_start<=97000;
    obs_data_set_string(cfg,"input_source","");obs_data_set_string(cfg,"alternate_prompt","glittering bright plucked synth and soft piano");
    obs_data_set_string(cfg,"lora_a",argv[5]);obs_data_set_double(cfg,"lora_a_strength",.75);
    obs_data_set_string(cfg,"lora_b",argv[5]);obs_data_set_double(cfg,"lora_b_strength",.5);
    obs_data_set_bool(cfg,"automate_b",true);obs_data_set_string(cfg,"sight_channel",sight_key.c_str());obs_source_update(s,cfg);
    bool automated=run("generate",2);
    v2={};snapshot=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v2",&v2);
    automated=automated&&snapshot&&calldata_int(&v2,"automated")==1;
    bool sight=snapshot&&calldata_int(&v2,"sight_cues")==1;calldata_free(&v2);
    obs_data_set_string(cfg,"lora_a","/nonexistent/test-adapter.gguf");obs_source_update(s,cfg);
    bool invalid_adapter=button(s,"generate")&&wait_status(s,"failed",3)&&status(s,"played")==count;
    // An unavailable/stale producer must fall back to the base prompt.
    v2={};snapshot=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v2",&v2);
    const auto cue_before=calldata_int(&v2,"sight_cues");calldata_free(&v2);
    fresh=false;obs_data_set_string(cfg,"lora_a","");obs_data_set_string(cfg,"lora_b","");
    obs_data_set_string(cfg,"alternate_prompt","");obs_source_update(s,cfg);
    bool stale=run("generate",2);
    v2={};snapshot=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v2",&v2);
    stale=stale&&snapshot&&cue_before>=1&&calldata_int(&v2,"sight_cues")==cue_before;calldata_free(&v2);
    printf("SA3_SIGHT_FIXTURE fresh=%d stale_fallback=%d\n",sight,stale);
    bool ok=sfx&&transform&&extend&&bad&&capped&&captured_ok&&automated&&invalid_adapter&&sight&&stale&&!c.bad;
    obs_source_filter_remove(video_source,motion_source);obs_source_release(motion_source);obs_source_release(video_source);
    printf("SA3_ADVANCED capture=%d captured=%lld automation=%d invalid_adapter=%d pass=%d\n",captured_ok,(long long)captured,automated,invalid_adapter,ok);
    obs_set_output_source(2,nullptr);obs_source_release(input);
    printf("SA3_OPERATIONS sfx=%d transform=%d extend=%d invalid_wav=%d cap=%d pass=%d\n",sfx,transform,extend,bad,capped,ok);
    obs_source_remove_audio_capture_callback(s,capture,&c);obs_set_output_source(1,nullptr);
    obs_source_release(s);obs_data_release(cfg);obs_wait_for_destroy_queue();obs_shutdown();
    return ok?0:1;
}
