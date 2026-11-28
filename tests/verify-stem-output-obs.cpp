#ifdef SOUND_RT_PROBE
#include "rt-callback-probe.hpp"
#endif
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>
#include <algorithm>

static const char* input_name(void*){return "Stem fanout test input";}
static void* input_create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void input_destroy(void*){}
static obs_source_info input_info={
    .id="sound_stem_fanout_test_input",.type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_AUDIO,.get_name=input_name,.create=input_create,.destroy=input_destroy,
};
struct Packet{
    uint64_t ts;
    uint32_t frames;
    bool operator==(const Packet& b)const{return ts==b.ts&&frames==b.frames;}
};
struct Stats{
    uint64_t frames=0;
    uint64_t late_end=22;
    double early=0,late_l=0,late_r=0,diff=0,total=0,fault=0,prefill=0,restored=0;
    std::vector<Packet> packets;
};
static void capture(void* ptr,obs_source_t*,const audio_data* audio,bool){
    auto& s=*static_cast<Stats*>(ptr);
    auto* l=reinterpret_cast<const float*>(audio->data[0]);
    auto* r=reinterpret_cast<const float*>(audio->data[1]);
    if(!l||!r)return;
    s.packets.push_back({audio->timestamp,audio->frames});
    for(size_t i=0;i<audio->frames;++i){
        double a=l[i],b=r[i];s.total+=a*a+b*b;
        if(s.frames+i<11*48000)s.early+=a*a+b*b;
        if(s.frames+i>=15*48000&&s.frames+i<25*48000)s.fault+=a*a+b*b;
        if(s.frames+i>=25*48000&&s.frames+i<37*48000)s.prefill+=a*a+b*b;
        if(s.frames+i>=38*48000&&s.frames+i<48*48000)s.restored+=a*a+b*b;
        if(s.frames+i>=13*48000&&s.frames+i<s.late_end*48000){
            s.late_l+=a*a;s.late_r+=b*b;s.diff+=(a-b)*(a-b);
        }
    }
    s.frames+=audio->frames;
}
int main(int argc,char**argv){
    if(argc!=5)return 2;
    const bool managed=std::string(argv[4])=="--managed";
    const bool fixture=std::getenv("SOUND_STEM_OUTPUT_FIXTURE");
    const bool recover=std::getenv("SOUND_STEM_OUTPUT_RECOVER");
    const bool smooth=std::getenv("SOUND_STEM_SMOOTH");
    const size_t seconds=managed?36:recover?49:24;
    std::ifstream in(argv[3],std::ios::binary);
    std::vector<float> pcm(24*48000*2);
    if(!in.read(reinterpret_cast<char*>(pcm.data()),pcm.size()*sizeof(float)))return 3;
    if(!obs_startup("en-US",nullptr,nullptr))return 4;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 5;
    obs_module_t* module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(module))return 6;
    auto* output_props=obs_get_source_properties("stem_live_output");
    if(!output_props){fprintf(stderr,"STEM_OUTPUT_NOT_REGISTERED\n");obs_shutdown();return 9;}
    obs_properties_destroy(output_props);
    obs_register_source(&input_info);
    auto* input=obs_source_create(input_info.id,"fixture",nullptr,nullptr);
    auto* settings=obs_data_create();
    obs_data_set_int(settings,"mix_mode",3);
    obs_data_set_bool(settings,"smooth_windows",smooth);
    if(!managed)obs_data_set_string(settings,"socket_path",argv[4]);
    auto* filter=obs_source_create("stem_live_buffered_filter","stem parent",settings,nullptr);
    if(!input||!filter)return 7;
    const char* uuid=obs_source_get_uuid(filter);
    if(!uuid||!*uuid)return 8;
    // Children exist before the producer is attached, as can happen on scene load.
    std::array<obs_source_t*,4> outputs{};
    std::array<Stats,4> stats{};
    if(managed)for(auto& s:stats)s.late_end=34;
    const char* names[]={"drums","bass","other","vocals"};
    for(size_t i=0;i<4;++i){
        auto* child=obs_data_create();
        obs_data_set_string(child,"filter_uuid",uuid);
        obs_data_set_int(child,"stem",i);
        outputs[i]=obs_source_create("stem_live_output",names[i],child,nullptr);
        obs_data_release(child);
        if(!outputs[i]){fprintf(stderr,"STEM_OUTPUT_NOT_REGISTERED\n");return 9;}
        obs_source_add_audio_capture_callback(outputs[i],capture,&stats[i]);
    }
    obs_source_filter_add(input,filter);
    auto* props=obs_source_properties(outputs[0]);
    auto* choices=props?obs_properties_get(props,"filter_uuid"):nullptr;
    bool selectable=false;
    if(choices)for(size_t i=0;i<obs_property_list_item_count(choices);++i){
        auto* value=obs_property_list_item_string(choices,i);
        selectable|=value&&std::string(value)==uuid;
    }
    if(props)obs_properties_destroy(props);
    auto* scene=obs_scene_create("stem scene");
    obs_scene_add(scene,input);
    for(auto* s:outputs)obs_scene_add(scene,s);
    auto* scene_source=obs_scene_get_source(scene);
    obs_set_output_source(1,scene_source); // test harness, not the plugin
    obs_source_inc_active(input);
    for(auto* s:outputs)obs_source_inc_active(s);
    Stats parent;
    obs_source_add_audio_capture_callback(input,capture,&parent);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto started=std::chrono::steady_clock::now();
    uint64_t initial=os_gettime_ns();
    std::array<float,480> l{},r{};
    for(size_t i=0;i<seconds*100;++i){
        if(i==1500){
            obs_source_set_name(filter,"renamed stem parent");
            if(recover){
                obs_data_set_string(settings,"socket_path","/sound-stem-output-test-absent.sock");
                obs_source_update(filter,settings);
            }
        }
        if(recover&&i==2500){
            obs_data_set_string(settings,"socket_path",argv[4]);obs_source_update(filter,settings);
        }
        for(size_t j=0;j<480;++j){
            auto at=((i*480+j)%(pcm.size()/2))*2;
            l[j]=pcm[at];r[j]=pcm[at+1];
        }
        obs_source_audio audio{};
        audio.data[0]=reinterpret_cast<uint8_t*>(l.data());audio.data[1]=reinterpret_cast<uint8_t*>(r.data());
        audio.frames=480;audio.speakers=SPEAKERS_STEREO;audio.format=AUDIO_FORMAT_FLOAT_PLANAR;
        audio.samples_per_sec=48000;audio.timestamp=initial+i*10000000ULL;
        obs_source_output_audio(input,&audio);
        std::this_thread::sleep_until(started+std::chrono::milliseconds((i+1)*10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for(size_t i=0;i<4;++i)obs_source_remove_audio_capture_callback(outputs[i],capture,&stats[i]);
    obs_source_remove_audio_capture_callback(input,capture,&parent);
    bool pass=selectable&&parent.frames==seconds*48000&&parent.total==0;
    auto* actual=obs_get_output_source(1);
    pass=pass&&actual==scene_source;obs_source_release(actual);
    for(size_t i=2;i<=5;++i){auto* s=obs_get_output_source(i);pass=pass&&!s;obs_source_release(s);}
    for(size_t i=0;i<4;++i){
        auto& s=stats[i];
        bool equal=s.packets==stats[0].packets;
        bool valid=s.frames==seconds*48000&&s.early==0&&s.late_l>0&&s.late_r>0&&s.diff>0&&equal;
        if(recover)valid=valid&&s.fault==0&&s.prefill==0&&s.restored>0;
        if(fixture&&!recover){
            double n=9*48000;
            valid=valid&&std::abs(s.late_l/n-std::pow(.025*(i+1),2))<1e-7
                       &&std::abs(s.late_r/n-std::pow(.0375*(i+1),2))<1e-7;
        }
        pass=pass&&valid;
        printf("STEM_OUTPUT stem=%zu frames=%llu timestamp_equal=%d early=%g stereo_energy=%g,%g diff=%g fault=%g prefill=%g restored=%g pass=%d\n",
            i,(unsigned long long)s.frames,equal,s.early,s.late_l,s.late_r,s.diff,s.fault,s.prefill,s.restored,valid);
    }
    calldata_t d{};proc_handler_call(obs_source_get_proc_handler(filter),"sound_stem_live_v1",&d);
    auto windows=calldata_int(&d,"windows"),retries=calldata_int(&d,"automatic_retries");calldata_free(&d);
    pass=pass&&(managed?(windows>=3&&retries==0):recover?(windows>=3&&retries>=1):(windows>=2&&windows<=(smooth?4:3)&&retries==0));
    calldata_t fanout{};
    bool fanout_status=proc_handler_call(obs_source_get_proc_handler(filter),"sound_stem_fanout_v1",&fanout);
    auto packets=calldata_int(&fanout,"packets"),drops=calldata_int(&fanout,"drops"),subscribers=calldata_int(&fanout,"subscribers");
    calldata_free(&fanout);
    pass=pass&&fanout_status&&packets==static_cast<long long>(seconds*100)&&drops==0&&subscribers==4;
    if(managed){
        calldata_t w{};bool state=proc_handler_call(obs_source_get_proc_handler(filter),"sound_stem_worker_v1",&w);
        auto* name=calldata_string(&w,"state");
        bool ready=state&&name&&std::string(name)=="Managed worker ready";
        printf("DEMUCS_MANAGED_READY=%d\n",ready);pass&=ready;calldata_free(&w);
    }
    // Disconnect the producer while all four ordinary outputs remain alive.
    // A child must not retain or resurrect the old model/queue.
    obs_source_filter_remove(input,filter);
    std::array<Stats,4> detached{};
    for(size_t i=0;i<4;++i)obs_source_add_audio_capture_callback(outputs[i],capture,&detached[i]);
    auto detach_start=std::chrono::steady_clock::now();
    for(size_t i=0;i<100;++i){
        obs_source_audio audio{};
        audio.data[0]=reinterpret_cast<uint8_t*>(l.data());audio.data[1]=reinterpret_cast<uint8_t*>(r.data());
        audio.frames=480;audio.speakers=SPEAKERS_STEREO;audio.format=AUDIO_FORMAT_FLOAT_PLANAR;
        audio.samples_per_sec=48000;audio.timestamp=initial+(seconds*100+i)*10000000ULL;
        obs_source_output_audio(input,&audio);
        std::this_thread::sleep_until(detach_start+std::chrono::milliseconds((i+1)*10));
    }
    bool detached_silent=true;
    for(size_t i=0;i<4;++i){
        obs_source_remove_audio_capture_callback(outputs[i],capture,&detached[i]);
        detached_silent&=detached[i].frames==0;
    }
    pass=pass&&detached_silent;
    printf("STEM_FANOUT_OBS recover=%d parent_silence=%d windows=%lld retries=%lld selectable=%d packets=%lld drops=%lld subscribers=%lld detached=%d pass=%d\n",
        recover,parent.total==0,(long long)windows,(long long)retries,selectable,(long long)packets,(long long)drops,(long long)subscribers,detached_silent,pass);
    obs_set_output_source(1,nullptr);
    obs_source_dec_active(input);
    for(auto* s:outputs)obs_source_dec_active(s);
    obs_source_release(filter);
    obs_scene_release(scene);
    for(auto* s:outputs)obs_source_release(s);
    obs_source_release(input);obs_data_release(settings);obs_shutdown();
#ifdef SOUND_RT_PROBE
    pass=rt_probe::report(1400,{4})&&pass;
#endif
    return pass?0:1;
}
