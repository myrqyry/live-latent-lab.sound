#ifdef SOUND_RT_PROBE
#include "rt-callback-probe.hpp"
#endif
// Actual OBS source/filter audio capture against the real local GPU sidecar.
// Uses a PRIVATE stereo music fixture converted to float32 in /tmp. Neither
// the fixture nor its generated stems enter the source repository.
#include <obs.h>
#include <obs-module.h>
#include <callback/proc.h>
#include <cstdlib>
#include <unistd.h>
#include <util/platform.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

struct Measure {
    uint64_t silent_until=12*48000;
    std::atomic<uint64_t> frames{0};
    std::atomic<double> total_energy{0},prefill_energy{0};
    std::atomic<double> early_energy{0},late_energy_l{0},late_energy_r{0};
    std::atomic<double> late_stereo_diff{0};
    std::atomic<uint64_t> late_nonzero{0};
};
static void add(std::atomic<double>& target,double value){
    double old=target.load();
    while(!target.compare_exchange_weak(old,old+value)){}
}
static void capture(void* param,obs_source_t*,const audio_data* packet,bool){
    auto& s=*static_cast<Measure*>(param);
    const float* l=reinterpret_cast<const float*>(packet->data[0]);
    const float* r=reinterpret_cast<const float*>(packet->data[1]);
    if(!l || !r)return;
    const uint64_t initial=s.frames.fetch_add(packet->frames);
    double early=0,late_l=0,late_r=0,diff=0,total=0,prefill=0;
    uint64_t nonzero=0;
    for(uint32_t i=0;i<packet->frames;++i){
        const uint64_t time=initial+i;
        total+=double(l[i])*l[i]+double(r[i])*r[i];
        if(time<s.silent_until)prefill+=double(l[i])*l[i]+double(r[i])*r[i];
        if(time<11*48000)early+=double(l[i])*l[i]+double(r[i])*r[i];
        if(time>=13*48000 && time<23*48000){
            late_l+=double(l[i])*l[i];
            late_r+=double(r[i])*r[i];
            diff+=double(l[i]-r[i])*(l[i]-r[i]);
            if(std::abs(l[i])>.0001f || std::abs(r[i])>.0001f)++nonzero;
        }
    }
    add(s.total_energy,total);add(s.prefill_energy,prefill);
    add(s.early_energy,early);add(s.late_energy_l,late_l);
    add(s.late_energy_r,late_r);add(s.late_stereo_diff,diff);
    s.late_nonzero+=nonzero;
}
static const char* source_name(void*){return "Sound live stem fixture";}
static void* source_create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void source_destroy(void*){}
static obs_source_info source_info={
    .id="sound_live_stem_test_input",
    .type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=source_name,
    .create=source_create,
    .destroy=source_destroy,
};
int main(int argc,char**argv){
    if(argc!=5 && argc!=6){
        std::fprintf(stderr,"usage: test MODULE.so DATA_DIR PCM_INTERLEAVED_F32 SIDECAR_SOCKET\n");
        return 2;
    }
    const std::string model_socket=argv[4];
    const bool expect_silence=argc==6 && std::string(argv[5])=="--expect-silence";
    const size_t total_frames=(expect_silence?14:24)*48000;
    std::ifstream in(argv[3],std::ios::binary);
    std::vector<float> pcm(total_frames*2);
    if(!in.read(reinterpret_cast<char*>(pcm.data()),
                std::streamsize(pcm.size()*sizeof(float))))return 3;
    if(!obs_startup("en-US",nullptr,nullptr))return 4;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 5;
    obs_module_t* module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS || !obs_init_module(module))
        return 6;
    obs_register_source(&source_info);
    auto* input=obs_source_create("sound_live_stem_test_input","fixture",nullptr,nullptr);
    auto* props=obs_get_source_properties("stem_live_buffered_filter");
    auto* selection=props?obs_properties_get(props,"stem"):nullptr;
    const bool properties=props&&selection&&obs_property_list_item_count(selection)==4;
    if(props)obs_properties_destroy(props);
    auto* settings=obs_data_create();
    // Private hard link to the test sidecar socket. Never rename or
    // remove the actual sidecar endpoint; create only our own alias.
    struct Alias{
        std::string dir,path;
        ~Alias(){if(!path.empty())unlink(path.c_str());if(!dir.empty())rmdir(dir.c_str());}
    } alias;
    const bool auto_recover=std::getenv("SOUND_STEM_AUTO_RECOVER")!=nullptr;
    if(auto_recover){
        std::string pattern=model_socket+".test-XXXXXX";
        if(!mkdtemp(pattern.data()))return 8;
        alias.dir=pattern;alias.path=pattern+"/endpoint.sock";
    }
    obs_data_set_string(settings,"socket_path",auto_recover?alias.path.c_str():model_socket.c_str());
    obs_data_set_int(settings,"stem",3); // Vocal stem; no dry duplication
    auto* filter=obs_source_create("stem_live_buffered_filter","live-vocal-test",settings,nullptr);
    if(!input || !filter)return 7;
    obs_source_filter_add(input,filter);
    obs_set_output_source(1,input);
    obs_source_set_audio_mixers(input,1);
    obs_source_inc_active(input);
    obs_source_set_audio_active(input,true);
    obs_source_set_async_unbuffered(input,true);
    uint64_t initial=os_gettime_ns(),sent=0;
    std::array<float,480> l{},r{};
    auto stream=[&](Measure& stats,size_t seconds){
        obs_source_add_audio_capture_callback(input,capture,&stats);
        auto wall=std::chrono::steady_clock::now();
        for(size_t round=0;round<seconds*100;++round){
            for(size_t i=0;i<480;++i){
                const auto at=((sent*480+i)%(pcm.size()/2))*2;
                l[i]=pcm[at];r[i]=pcm[at+1];
            }
            obs_source_audio audio{};
            audio.data[0]=reinterpret_cast<uint8_t*>(l.data());
            audio.data[1]=reinterpret_cast<uint8_t*>(r.data());
            audio.frames=480;audio.speakers=SPEAKERS_STEREO;
            audio.format=AUDIO_FORMAT_FLOAT_PLANAR;audio.samples_per_sec=48000;
            audio.timestamp=initial+sent++*10000000ULL;
            obs_source_output_audio(input,&audio);
            std::this_thread::sleep_until(wall+std::chrono::milliseconds((round+1)*10));
        }
        obs_source_remove_audio_capture_callback(input,capture,&stats);
    };
    struct Status{std::string status;long long windows=0,retries=0;};
    auto snapshot=[&]{
        Status out;calldata_t d{};
        if(proc_handler_call(obs_source_get_proc_handler(filter),"sound_stem_live_v1",&d)){
            if(auto* name=calldata_string(&d,"status"))out.status=name;
            out.windows=calldata_int(&d,"windows");out.retries=calldata_int(&d,"automatic_retries");
        }
        calldata_free(&d);return out;
    };
    if(auto_recover){
        Measure stats;stats.silent_until=20*48000;
        stream(stats,9);auto fault=snapshot();
        bool attached=link(model_socket.c_str(),alias.path.c_str())==0;
        stream(stats,24);auto last=snapshot();
        bool pass=properties&&fault.retries==1&&attached&&
            stats.frames>=32*48000&&stats.prefill_energy==0&&
            stats.late_energy_l>5&&stats.late_energy_r>5&&stats.late_stereo_diff>1&&
            last.status=="ready"&&last.retries==1&&last.windows>=2;
        printf("STEM_LIVE_AUTO_RECOVER attached=%d retries=%lld silent_20s_energy=%g recovered_windows=%lld wet=%g,%g pass=%d\n",
            attached,last.retries,stats.prefill_energy.load(),last.windows,stats.late_energy_l.load(),stats.late_energy_r.load(),pass);
        obs_set_output_source(1,nullptr);obs_source_dec_active(input);
        obs_source_filter_remove(input,filter);obs_source_release(filter);obs_source_release(input);
        obs_data_release(settings);obs_shutdown();
#ifdef SOUND_RT_PROBE
        pass=rt_probe::report(1400,{4})&&pass;
#endif
        return pass?0:1;
    }
    if(std::getenv("SOUND_STEM_RECOVER")){
        Measure before,failed,recovered;
        stream(before,15);
        auto first=snapshot();
        obs_data_set_string(settings,"socket_path","/sound-stem-test-absent.sock");
        obs_source_update(filter,settings);
        stream(failed,10);
        auto fault=snapshot();
        obs_data_set_string(settings,"socket_path",model_socket.c_str());
        obs_source_update(filter,settings);
        stream(recovered,24);
        auto last=snapshot();
        bool pass=properties&&first.status=="ready"&&first.windows>=1&&before.total_energy>5&&
            failed.frames>=10*48000&&failed.total_energy==0&&fault.retries>=1&&
            recovered.frames>=23*48000&&recovered.prefill_energy==0&&
            recovered.late_energy_l>5&&recovered.late_energy_r>5&&recovered.late_stereo_diff>1&&
            last.status=="ready"&&last.windows>=first.windows+2;
        printf("STEM_LIVE_RECOVER first=%s before_windows=%lld fault_retries=%lld muted_energy=%g prefill_energy=%g recovered_windows=%lld wet=%g,%g pass=%d\n",
            first.status.c_str(),first.windows,fault.retries,failed.total_energy.load(),recovered.prefill_energy.load(),
            last.windows,recovered.late_energy_l.load(),recovered.late_energy_r.load(),pass);
        obs_set_output_source(1,nullptr);obs_source_dec_active(input);
        obs_source_filter_remove(input,filter);obs_source_release(filter);obs_source_release(input);
        obs_data_release(settings);obs_shutdown();
#ifdef SOUND_RT_PROBE
        pass=rt_probe::report(1400,{4})&&pass;
#endif
        return pass?0:1;
    }
    Measure stats;
    stream(stats,expect_silence?14:24);
    const auto n=stats.frames.load();
    const double early=stats.early_energy.load();
    const double late_l=stats.late_energy_l.load();
    const double late_r=stats.late_energy_r.load();
    const double diff=stats.late_stereo_diff.load();
    const bool pass=expect_silence
        ? properties&&n>=13*48000 && early<1e-6 && late_l<1e-6 && late_r<1e-6 &&
          stats.late_nonzero.load()==0
        : properties&&n>=19*48000 && early<1e-6 && late_l>5.0 && late_r>5.0 &&
          diff>1.0 && stats.late_nonzero.load()>48000;
    std::printf("STEM_LIVE_OBS_REAL_AUDIO expect_silence=%d frames=%llu first_11s_energy=%g "
                "late_l=%g late_r=%g stereo_diff=%g nonzero=%llu "
                "properties=%d pass=%d\n",
        int(expect_silence),(unsigned long long)n,early,late_l,late_r,diff,
        (unsigned long long)stats.late_nonzero.load(),properties,pass);
    obs_set_output_source(1,nullptr);
    obs_source_dec_active(input);
    obs_source_filter_remove(input,filter);
    obs_source_release(filter);obs_source_release(input);
    obs_data_release(settings);
    obs_shutdown();
#ifdef SOUND_RT_PROBE
    const bool probe_ok=rt_probe::report(1400,{4});
    return pass&&probe_ok?0:1;
#else
    return pass?0:1;
#endif
}
