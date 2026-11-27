#ifdef SOUND_RT_PROBE
#include "rt-capture-probe.hpp"
#endif
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <algorithm>
#include <vector>
#include <fstream>
using namespace std::chrono_literals;
static const char* name(void*){return "SAME live fixture";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info info={.id="same_live_fixture",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=create,.destroy=destroy};
struct Stats{
    std::atomic<uint64_t> frames{0},bad{0},nonzero{0},first{0},last{0},gap{0};
    std::atomic<double> peak_step{0};
    uint32_t hz=44100;uint64_t expected=0;std::atomic<uint64_t> max_gap{0};
    float prev=0;bool seen=false;
    std::vector<float> recording;size_t written=0;
};
static void capture(void* ptr,obs_source_t*,const audio_data* a,bool){
    auto& s=*static_cast<Stats*>(ptr);
    if(!a||!a->data[0]||!a->data[1]){++s.bad;return;}
    auto* l=reinterpret_cast<const float*>(a->data[0]);
    auto* r=reinterpret_cast<const float*>(a->data[1]);
    for(uint32_t i=0;i<a->frames;++i){
        if(!std::isfinite(l[i])||!std::isfinite(r[i])||std::abs(l[i])>1.01f||std::abs(r[i])>1.01f)++s.bad;
        if(std::abs(l[i])>.001f)++s.nonzero;
        if(s.seen)s.peak_step.store(std::max(s.peak_step.load(),double(std::abs(l[i]-s.prev))));
        s.prev=l[i];s.seen=true;
        if(s.written+2<=s.recording.size()){s.recording[s.written++]=l[i];s.recording[s.written++]=r[i];}
    }
    if(!s.first)s.first=a->timestamp;
    const auto last=s.last.exchange(a->timestamp);
    if(last&&a->timestamp<last)++s.gap;
    if(s.expected){
        const auto delta=a->timestamp>s.expected?a->timestamp-s.expected:s.expected-a->timestamp;
        s.max_gap.store(std::max(s.max_gap.load(),delta));
        if(delta>2000000)++s.gap;
    }
    s.expected=a->timestamp+uint64_t(a->frames)*1000000000/s.hz;
    s.frames+=a->frames;
}
static bool button(obs_source_t* s,const char* key){
    auto* p=obs_source_properties(s);auto* b=obs_properties_get(p,key);
    if(!b){auto* group=obs_properties_get(p,"latent_morph");if(group)b=obs_properties_get(obs_property_group_content(group),key);}
    if(b)obs_property_button_clicked(b,s);obs_properties_destroy(p);return b!=nullptr;
}
static long long get(obs_source_t* s,const char* proc,const char* key){
    calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),proc,&d);
    auto n=ok?calldata_int(&d,key):-1;calldata_free(&d);return n;
}
int main(int argc,char** argv){
    if(argc!=4||!obs_startup("en-US",nullptr,nullptr))return 2;
    const uint32_t hz=std::getenv("SAME_LIVE_44100")?44100:48000;
    obs_audio_info ai{};ai.samples_per_sec=hz;ai.speakers=SPEAKERS_STEREO;if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_module_t* m=nullptr;if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 5;
    obs_register_source(&info);
    auto* source=obs_source_create("same_live_fixture","Input",nullptr,nullptr);
    auto* d=obs_data_create();obs_data_set_string(d,"models_dir",argv[3]);
    obs_data_set_string(d,"input_source",obs_source_get_uuid(source));
    obs_data_set_string(d,"morph_effect","smear");obs_data_set_double(d,"morph_mix",.65);
    obs_data_set_int(d,"live_reserve",std::getenv("SAME_LIVE_COMPETE")?8:4);
    auto* wet=obs_source_create("sound_audio_diffusion_source","Wet",d,nullptr);
    if(!wet)return 6;
    Stats dry,out;dry.hz=out.hz=hz;
    std::vector<float> music;
    if(const char* path=std::getenv("SAME_LIVE_MUSIC_RAW")){
        std::ifstream f(path,std::ios::binary|std::ios::ate);
        auto bytes=f.tellg();if(bytes<=0||bytes>std::streamoff(48000*60*8)||bytes%8)return 9;
        music.resize(size_t(bytes)/4);f.seekg(0);if(!f.read(reinterpret_cast<char*>(music.data()),bytes))return 9;
    }
    if(std::getenv("SAME_LIVE_RECORD_PREFIX")){dry.recording.resize(size_t(hz)*30*2);out.recording.resize(size_t(hz)*30*2);}
    obs_source_add_audio_capture_callback(source,capture,&dry);obs_source_add_audio_capture_callback(wet,capture,&out);
    obs_set_output_source(1,source);obs_set_output_source(2,wet);
    obs_source_set_audio_active(source,true);obs_source_set_audio_active(wet,true);
    std::this_thread::sleep_for(350ms);
    bool start=button(wet,"live_texture");
    const bool supported=get(wet,"sound_same_live_v1","blocks")>=0;
    if(!start||!supported){puts("SAME_LIVE missing");return 8;}
    std::atomic<bool> running{true};
    const auto t0=std::chrono::steady_clock::now();
    std::thread feed([&]{
        std::array<float,480> l{},r{};auto frames=hz/100;auto ts=os_gettime_ns();
        for(uint64_t k=0;running;++k){
            for(unsigned j=0;j<frames;++j){
                auto t=double(k*frames+j)/hz;
                l[j]=float(.2*std::sin(6.28318530718*(220*t+3*t*t))+.08*std::sin(6.28318530718*659*t));
                r[j]=l[j]*.6f;
                if(!music.empty()){const auto index=2*((k*frames+j)%(music.size()/2));l[j]=music[index]*.5f;r[j]=music[index+1]*.5f;}
            }
            obs_source_audio a{};a.data[0]=reinterpret_cast<uint8_t*>(l.data());a.data[1]=reinterpret_cast<uint8_t*>(r.data());a.frames=frames;a.samples_per_sec=hz;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;a.timestamp=ts+k*10000000;
            obs_source_output_audio(source,&a);std::this_thread::sleep_until(t0+(k+1)*10ms);
        }
    });
    auto* other=static_cast<obs_source_t*>(nullptr);
    bool competing=std::getenv("SAME_LIVE_COMPETE")!=nullptr;
    bool competitor_ok=!competing;
    const int seconds=std::getenv("SAME_LIVE_SOAK")?40:competing?14:9;
    for(int i=0;i<seconds*10;++i){
        if(competing&&i==55){
            auto* cfg=obs_data_create();obs_data_set_string(cfg,"models_dir",argv[3]);obs_data_set_double(cfg,"duration",60);obs_data_set_int(cfg,"steps",32);obs_data_set_int(cfg,"seed",17);
            other=obs_source_create("sound_audio_diffusion_source","Competing generation",cfg,nullptr);obs_data_release(cfg);
            obs_set_output_source(3,other);obs_source_set_audio_active(other,true);
            std::this_thread::sleep_for(350ms);button(other,"generate");
        }
        if(i==65){obs_data_set_string(d,"morph_effect","grain");obs_data_set_double(d,"morph_mix",.6);obs_source_update(wet,d);}
        std::this_thread::sleep_for(100ms);
    }
    if(other){
        auto started=get(other,"sound_diffusion_status_v3","started"),completed=get(other,"sound_diffusion_status_v3","completed"),failed=get(other,"sound_diffusion_status_v3","failed");
        calldata_t c{};proc_handler_call(obs_source_get_proc_handler(other),"sound_diffusion_status_v3",&c);
        printf("SAME_LIVE_COMPETITOR started=%lld completed=%lld failed=%lld message=%s\n",started,completed,failed,calldata_string(&c,"message"));calldata_free(&c);
        competitor_ok=started>=1&&completed>=1&&failed==0;
    }
    auto blocks=get(wet,"sound_same_live_v1","blocks"),dropped=get(wet,"sound_same_live_v1","dropped"),delay=get(wet,"sound_same_live_v1","delay_ms");
    const auto original_gap=out.gap.load();const auto original_step=out.peak_step.load();
    auto failures=get(wet,"sound_diffusion_status_v3","failed");
    auto underruns=get(wet,"sound_diffusion_status_v3","underruns");
    const bool stall=std::getenv("SAME_LIVE_STALL")!=nullptr;
    bool stall_ok=true;
    if(stall){
        running=false;feed.join();
        for(int i=0;i<60&&(get(wet,"sound_diffusion_status_v3","failed")==0||get(wet,"sound_same_resident_v1","active")!=0);++i)std::this_thread::sleep_for(100ms);
        auto fail_count=get(wet,"sound_diffusion_status_v3","failed"),active=get(wet,"sound_same_resident_v1","active");
        auto silent_frames=out.frames.load();std::this_thread::sleep_for(250ms);
        stall_ok=fail_count==1&&active==0&&out.frames==silent_frames;
        printf("SAME_LIVE_STALL_STATUS failures=%lld active=%lld stopped_output=%d\n",fail_count,active,int(out.frames==silent_frames));
        auto before=out.frames.load();
        // Start a fresh callback session with a new timestamp origin.
        start=button(wet,"live_texture");
        std::thread again([&]{
            std::array<float,480> l{},r{};auto frames=hz/100;auto ts=os_gettime_ns();
            auto begin=std::chrono::steady_clock::now();
            for(unsigned k=0;k<350;++k){
                for(unsigned j=0;j<frames;++j){l[j]=.2f*std::sin(6.28318530718*440.*double(k*frames+j)/hz);r[j]=l[j]*.5f;}
                obs_source_audio a{};a.data[0]=reinterpret_cast<uint8_t*>(l.data());a.data[1]=reinterpret_cast<uint8_t*>(r.data());a.frames=frames;a.samples_per_sec=hz;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;a.timestamp=ts+uint64_t(k)*10000000;
                obs_source_output_audio(source,&a);std::this_thread::sleep_until(begin+(k+1)*10ms);
            }
        });
        again.join();
        const bool recovered=start&&out.frames>before+hz/2&&get(wet,"sound_same_live_v1","blocks")>=4;
        printf("SAME_LIVE_STALL failed_closed=%d recovered=%d\n",stall_ok,recovered);
        stall_ok=stall_ok&&recovered;
    }else{running=false;feed.join();}
    button(wet,"stop");
    std::this_thread::sleep_for(150ms);const auto after=out.frames.load();std::this_thread::sleep_for(400ms);
    bool stopped=out.frames==after&&get(wet,"sound_same_resident_v1","active")==0;
    bool ok=blocks>=8&&out.frames>hz*3&&out.nonzero>1000&&!out.bad&&!original_gap&&original_step<.2&&dry.frames>hz*7&&!dry.bad&&
        dropped==0&&delay>0&&delay<6000&&failures==0&&underruns==0&&stopped&&competitor_ok&&stall_ok;
    printf("SAME_LIVE rate=%u blocks=%lld output=%llu dry=%llu delay_ms=%lld dropped=%lld failed=%lld underruns=%lld max_step=%.4f gap_ns=%llu stopped=%d competitor=%d pass=%d\n",hz,blocks,(unsigned long long)out.frames.load(),(unsigned long long)dry.frames.load(),delay,dropped,failures,underruns,out.peak_step.load(),(unsigned long long)out.max_gap.load(),stopped,competitor_ok,ok);
    if(other){button(other,"stop");obs_set_output_source(3,nullptr);obs_source_release(other);}
    obs_source_remove_audio_capture_callback(source,capture,&dry);obs_source_remove_audio_capture_callback(wet,capture,&out);
    if(const char* prefix=std::getenv("SAME_LIVE_RECORD_PREFIX")){
        for(auto item:{std::make_pair(&dry,"-dry.raw"),std::make_pair(&out,"-wet.raw")}){
            std::ofstream f(std::string(prefix)+item.second,std::ios::binary);
            f.write(reinterpret_cast<const char*>(item.first->recording.data()),item.first->written*4);
            if(!f)ok=false;
        }
    }
    obs_set_output_source(1,nullptr);obs_set_output_source(2,nullptr);obs_source_release(source);obs_source_release(wet);obs_data_release(d);obs_wait_for_destroy_queue();obs_shutdown();
#ifdef SOUND_RT_PROBE
    ok=rt_capture_probe::report(200)&&ok;
#endif
    return ok?0:1;
}
