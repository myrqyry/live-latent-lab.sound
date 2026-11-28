// Real libobs capture/fanout and sample-count latency test. Use a real v2
// CUDA sidecar, no mock separation. No changes to the normal OBS profile.
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
#include <string>
#include <cassert>
#include <filesystem>
#include <util/config-file.h>
#include <util/bmem.h>

static const char* input_name(void*){return "MelBand test input";}
static void* input_create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void input_destroy(void*){}
static obs_source_info input_info={
    .id="sound_melband_test_input",.type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_AUDIO,.get_name=input_name,.create=input_create,.destroy=input_destroy,
};
struct Packet{uint64_t ts;uint32_t frames;bool operator==(const Packet& b)const{return ts==b.ts&&frames==b.frames;}};
struct Capture{
    std::vector<float> l,r;
    std::vector<Packet> packets;
};
static void capture(void* ptr,obs_source_t*,const audio_data* a,bool){
    auto& s=*static_cast<Capture*>(ptr);
    auto* l=reinterpret_cast<const float*>(a->data[0]);auto* r=reinterpret_cast<const float*>(a->data[1]);
    if(!l||!r)return;
    s.packets.push_back({a->timestamp,a->frames});
    s.l.insert(s.l.end(),l,l+a->frames);s.r.insert(s.r.end(),r,r+a->frames);
}
static double energy(const Capture& s,size_t a,size_t b){
    double sum=0;
    for(size_t i=a;i<b&&i<s.l.size();++i)sum+=double(s.l[i])*s.l[i]+double(s.r[i])*s.r[i];
    return sum;
}
int main(int argc,char** argv){
    if(argc!=5)return 2;
    const bool managed=std::string(argv[4])=="--managed";
    const char* native_library=std::getenv("SOUND_MELBAND_NATIVE_LIBRARY");
    const char* fresh_model=std::getenv("SOUND_MELBAND_FRESH_MODEL");
    if((native_library||fresh_model)&&!managed)return 2;
    if(native_library&&fresh_model)return 2;
    const bool recover=std::getenv("SOUND_MELBAND_RECOVER");
    const bool mix=std::getenv("SOUND_MELBAND_MIX");
    if((mix&&recover)||(managed&&(mix||recover)))return 2;
    const uint32_t rate=std::getenv("SOUND_MELBAND_44100")?44100:48000;
    const size_t seconds=managed?24:recover?16:10,packet=rate/100,delay=rate*21/10;
    std::ifstream in(argv[3],std::ios::binary);
    std::vector<float> pcm(24*48000*2),orig_l(seconds*rate),orig_r(seconds*rate);
    if(!in.read(reinterpret_cast<char*>(pcm.data()),pcm.size()*sizeof(float)))return 3;
    for(size_t i=0;i<orig_l.size();++i){
        size_t src=i*48000/rate;
        orig_l[i]=pcm[2*src];orig_r[i]=pcm[2*src+1];
    }
    if(!obs_startup("en-US",std::getenv("SOUND_MELBAND_OBS_CONFIG"),nullptr))return 4;
    obs_audio_info ai{};ai.samples_per_sec=rate;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 5;
    obs_module_t* module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(module))return 6;
    if(native_library||fresh_model){
        if(!std::getenv("SOUND_MELBAND_OBS_CONFIG"))return 2;
        auto* path=obs_module_get_config_path(module,"preferences.ini");
        if(!path)return 6;
        namespace fs=std::filesystem;
        const auto config_path=fs::u8path(path);bfree(path);
        const auto local=config_path.parent_path()/"models";
        fs::path expected;
        if(fresh_model){
            if(fs::exists(config_path)||fs::exists(local))return 6;
            expected=local;
        }else{
            fs::create_directories(config_path.parent_path());
            config_t* pref=nullptr;
            if(config_open(&pref,config_path.u8string().c_str(),CONFIG_OPEN_ALWAYS)!=CONFIG_SUCCESS)return 6;
            config_set_string(pref,"General","models",native_library);
            bool saved=config_save(pref)==CONFIG_SUCCESS;config_close(pref);
            if(!saved)return 6;
            expected=fs::u8path(native_library)/"Mel-Band-RoFormer-GGUF";
            if(!fs::is_directory(expected))expected=fs::u8path(native_library);
        }
        auto* def=obs_get_source_defaults("stem_live_melband_filter");
        bool enabled=def&&obs_data_get_bool(def,"native_backend")&&std::string(obs_data_get_string(def,"native_model")).empty();
        obs_data_release(def);
        auto* props=obs_get_source_properties("stem_live_melband_filter");
        auto* picker=props?obs_properties_get(props,"native_model"):nullptr;
        const char* initial=picker?obs_property_path_default_path(picker):nullptr;
        bool rooted=initial&&fs::u8path(initial)==expected&&fs::is_directory(expected);
        if(props)obs_properties_destroy(props);
        if(!enabled||!rooted)return 6;
        if(fresh_model){fs::create_symlink(fs::absolute(fs::u8path(fresh_model)),local/"mel-band-roformer-f16.gguf");if(fs::exists(config_path))return 6;}
        printf("MELBAND_NATIVE_DEFAULT picker=%d enabled=%d\n",rooted,enabled);
    }
    obs_register_source(&input_info);
    auto* input=obs_source_create(input_info.id,"fixture",nullptr,nullptr);
    auto* settings=obs_data_create();
    obs_data_set_int(settings,"mix_mode",3);if(!managed)obs_data_set_string(settings,"socket_path",argv[4]);
    auto* filter=obs_source_create("stem_live_melband_filter","RoFormer parent",settings,nullptr);
    if(!input||!filter||!obs_source_get_uuid(filter))return 7;
    std::string uuid=obs_source_get_uuid(filter);
    std::array<obs_source_t*,3> outputs{};
    std::array<Capture,3> stats{};
    const int ids[]={3,4,0};const char* names[]={"vocals","instrumental","incompatible drums"};
    auto* child=obs_data_create();
    obs_data_set_string(child,"filter_uuid",uuid.c_str());
    for(size_t i=0;i<3;++i){
        obs_data_set_int(child,"stem",ids[i]);
        outputs[i]=obs_source_create("stem_live_output",names[i],child,nullptr);
        if(!outputs[i])return 8;
        obs_source_add_audio_capture_callback(outputs[i],capture,&stats[i]);
    }
    obs_source_filter_add(input,filter);
    auto* props=obs_source_properties(outputs[0]);
    auto* parents=props?obs_properties_get(props,"filter_uuid"):nullptr;
    auto* stem=props?obs_properties_get(props,"stem"):nullptr;
    bool picker=false;
    if(parents&&stem){
        for(size_t i=0;i<obs_property_list_item_count(parents);++i){
            auto* key=obs_property_list_item_string(parents,i);
            picker|=key&&uuid==key;
        }
        obs_property_modified(parents,child);
        for(size_t i=0;i<obs_property_list_item_count(stem);++i){
            auto id=obs_property_list_item_int(stem,i);
            picker&=obs_property_list_item_disabled(stem,i)==(id<3);
        }
    }
    if(props)obs_properties_destroy(props);
    obs_data_release(child);
    auto* scene=obs_scene_create("melband scene");obs_scene_add(scene,input);
    for(auto* s:outputs)obs_scene_add(scene,s);
    obs_set_output_source(1,obs_scene_get_source(scene));
    obs_source_inc_active(input);
    for(auto* s:outputs)obs_source_inc_active(s);
    Capture parent;obs_source_add_audio_capture_callback(input,capture,&parent);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto started=std::chrono::steady_clock::now();
    auto initial=os_gettime_ns();
    std::vector<float> l(packet),r(packet);
    for(size_t i=0;i<seconds*100;++i){
        if(mix&&i>=300&&i<=900&&i%100==0){
            int mode=i==300?0:i==400?1:i==800?3:2;
            obs_data_set_int(settings,"mix_mode",mode);
            obs_data_set_int(settings,"stem",3);
            obs_data_set_double(settings,"vocals_gain",i==500?.5:1.);
            obs_data_set_double(settings,"instrumental_gain",i==500?.25:1.);
            obs_data_set_bool(settings,"vocals_solo",i==600||i==700);
            obs_data_set_bool(settings,"vocals_mute",i==700);
            obs_source_update(filter,settings);
        }
        if(i==500){
            obs_source_set_name(filter,"renamed RoFormer parent");
            if(recover){
                obs_data_set_string(settings,"socket_path","/sound-melband-test-absent.sock");
                obs_source_update(filter,settings);
            }
        }
        if(recover&&i==900){
            obs_data_set_string(settings,"socket_path",argv[4]);obs_source_update(filter,settings);
        }
        std::copy_n(orig_l.data()+i*packet,packet,l.data());std::copy_n(orig_r.data()+i*packet,packet,r.data());
        obs_source_audio a{};
        a.data[0]=reinterpret_cast<uint8_t*>(l.data());a.data[1]=reinterpret_cast<uint8_t*>(r.data());
        a.frames=packet;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;
        a.samples_per_sec=rate;a.timestamp=initial+i*10000000ULL;
        obs_source_output_audio(input,&a);
        std::this_thread::sleep_until(started+std::chrono::milliseconds((i+1)*10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    for(size_t i=0;i<3;++i)obs_source_remove_audio_capture_callback(outputs[i],capture,&stats[i]);
    obs_source_remove_audio_capture_callback(input,capture,&parent);
    bool pass=picker&&stats[2].l.empty()&&stats[0].packets==stats[1].packets&&
        stats[0].l.size()==seconds*rate&&stats[1].l.size()==seconds*rate&&parent.l.size()==seconds*rate&&
        (mix?energy(parent,0,delay)==0:energy(parent,0,parent.l.size())==0);
    size_t first=delay;
    if(managed&&pass){
        while(first<seconds*rate&&stats[0].l[first]==0&&stats[0].r[first]==0&&stats[1].l[first]==0&&stats[1].r[first]==0)++first;
        pass=first>=delay&&first<seconds*rate-3*rate;
    }
    double maximum=0;
    if(pass){
        // First generation: complementary readout should reconstruct exact
        // delayed stereo mixture. All future/history and blend indices agree.
        for(size_t i=first;i<(recover?5:seconds)*rate;++i){
            maximum=std::max(maximum,double(std::abs(stats[0].l[i]+stats[1].l[i]-orig_l[i-delay])));
            maximum=std::max(maximum,double(std::abs(stats[0].r[i]+stats[1].r[i]-orig_r[i-delay])));
        }
        pass=maximum<2e-6;
    }
    if(mix&&pass){
        const std::array<std::array<float,2>,7> expected={{{1,0},{0,1},{.5,.25},{1,0},{0,0},{0,0},{1,1}}};
        double err=0;
        for(size_t mode=0;mode<expected.size();++mode){
            auto gains=expected[mode];
            for(size_t i=(mode+3)*rate+rate/10;i<(mode+4)*rate-rate/10;++i){
                err=std::max(err,double(std::abs(parent.l[i]-(stats[0].l[i]*gains[0]+stats[1].l[i]*gains[1]))));
                err=std::max(err,double(std::abs(parent.r[i]-(stats[0].r[i]*gains[0]+stats[1].r[i]*gains[1]))));
            }
        }
        pass=err<2e-6;
        printf("MELBAND_LIVE_MIX modes=7 max_error=%g pass=%d\n",err,pass);
    }
    for(size_t i=0;i<2;++i){
        auto& s=stats[i];
        bool valid=energy(s,0,delay)==0&&(managed?energy(s,rate*21,rate*23)>0:energy(s,rate*3,rate*5)>0);
        if(recover)valid=valid&&energy(s,rate*51/10,rate*9)==0&&energy(s,rate*9,rate*111/10)==0&&energy(s,rate*12,rate*15)>0;
        pass&=valid;
        printf("MELBAND_OBS stem=%s frames=%zu early=%g initial=%g fault=%g fresh_prefill=%g restored=%g pass=%d\n",
            names[i],s.l.size(),energy(s,0,delay),energy(s,3*rate,5*rate),energy(s,rate*51/10,9*rate),
            energy(s,9*rate,rate*111/10),energy(s,12*rate,15*rate),valid);
    }
    calldata_t d{};bool status=proc_handler_call(obs_source_get_proc_handler(filter),"sound_stem_live_v2",&d);
    auto windows=calldata_int(&d,"windows"),retries=calldata_int(&d,"automatic_retries"),delay_ms=calldata_int(&d,"delay_ms");calldata_free(&d);
    calldata_t f{};bool fan=proc_handler_call(obs_source_get_proc_handler(filter),"sound_stem_fanout_v1",&f);
    auto packets=calldata_int(&f,"packets"),drops=calldata_int(&f,"drops"),subscribers=calldata_int(&f,"subscribers");calldata_free(&f);
    if(managed){
        calldata_t w{};bool state=proc_handler_call(obs_source_get_proc_handler(filter),"sound_stem_worker_v1",&w);
        auto* name=calldata_string(&w,"state");
        bool ready=state&&name&&std::string(name)=="Managed worker ready";
        const char* detail=calldata_string(&w,"detail");
        if(native_library||fresh_model)ready=ready&&detail&&std::string(detail)=="Native CUDA GGUF";
        printf("MELBAND_MANAGED_START first_wet_s=%g ready=%d backend=%s\n",double(first)/rate,ready,detail?detail:"");
        pass=pass&&ready;calldata_free(&w);
    }
    pass=pass&&status&&fan&&delay_ms==2100&&windows>=8&&(recover?retries>=1:retries==0)&&packets==static_cast<long long>(seconds*100)&&drops==0&&subscribers==2;
    printf("MELBAND_OBS_SUMMARY rate=%u recovery=%d delay_ms=%lld windows=%lld retries=%lld packets=%lld drops=%lld subscribers=%lld picker=%d wrong_stem_silent=%d reconstruction_max=%g pass=%d\n",
        rate,recover,(long long)delay_ms,(long long)windows,(long long)retries,(long long)packets,(long long)drops,(long long)subscribers,picker,stats[2].l.empty(),maximum,pass);
    obs_set_output_source(1,nullptr);obs_source_dec_active(input);
    for(auto* s:outputs)obs_source_dec_active(s);
    obs_source_filter_remove(input,filter);obs_source_release(filter);obs_scene_release(scene);
    for(auto* s:outputs)obs_source_release(s);
    obs_source_release(input);obs_data_release(settings);obs_shutdown();
#ifdef SOUND_RT_PROBE
    pass=rt_probe::report(seconds*100,{7})&&pass;
#endif
    return pass?0:1;
}
