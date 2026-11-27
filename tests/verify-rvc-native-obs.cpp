#ifdef SOUND_RT_PROBE
#include "rt-callback-probe.hpp"
#endif
#include "audio-dispatch-timing.hpp"
#include <obs.h>
#include <obs-module.h>
#include <callback/proc.h>
#include <util/platform.h>
#include <array>
#include <vector>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <string>
#include <fstream>
#include <filesystem>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
struct Stats{
    std::vector<float> l,r;size_t count=0;bool invalid=false;
    explicit Stats(size_t n):l(n),r(n){}
};
static void capture(void* ptr,obs_source_t*,const audio_data* a,bool){
    auto& s=*static_cast<Stats*>(ptr);
    if(!a->data[0]||!a->data[1]){s.invalid=true;return;}
    auto* l=reinterpret_cast<const float*>(a->data[0]);auto* r=reinterpret_cast<const float*>(a->data[1]);
    for(size_t i=0;i<a->frames&&s.count<s.l.size();++i,++s.count){
        s.l[s.count]=l[i];s.r[s.count]=r[i];
        if(!std::isfinite(l[i])||!std::isfinite(r[i])||std::abs(l[i])>1||std::abs(r[i])>1)s.invalid=true;
    }
}
static const char* name(void*){return "RVC fixture";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info fixture={.id="rvc_fixture",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=create,.destroy=destroy};
struct Result{std::string status;long long hops=0;double pitch=0,ms=0;};
static Result snapshot(obs_source_t* f){
    calldata_t d{};Result r;
    if(proc_handler_call(obs_source_get_proc_handler(f),"sound_rvc_native_v1",&d)){
        const char* s=calldata_string(&d,"status");if(s)r.status=s;
        r.hops=calldata_int(&d,"hops");r.pitch=calldata_float(&d,"pitch_hz");r.ms=calldata_float(&d,"processing_ms");
    }
    calldata_free(&d);return r;
}
static double rms(const Stats& s,size_t rate,bool stereo_difference=false){
    if(s.count<=rate/2)return 0;double power=0;
    for(size_t i=rate/2;i<s.count;++i){double v=stereo_difference?s.l[i]-s.r[i]:s.l[i];power+=v*v;}
    return std::sqrt(power/(s.count-rate/2));
}
// Select only a direct child of this isolated libobs test process, with
// the expected executable. Signal via pidfd so PID reuse cannot redirect
// the injected crash to another process.
static bool crash_owned_helper(const char* executable){
    namespace fs=std::filesystem;
    std::error_code error;
    auto expected=fs::canonical(executable,error);if(error)return false;
    for(const auto& task:fs::directory_iterator("/proc/self/task",error)){
        std::ifstream children(task.path()/"children");int pid;
        while(children>>pid){
            if(pid<=1)continue;
            int fd=int(syscall(SYS_pidfd_open,pid,0));if(fd<0)continue;
            auto actual=fs::canonical("/proc/"+std::to_string(pid)+"/exe",error);
            bool ok=!error&&actual==expected&&syscall(SYS_pidfd_send_signal,fd,SIGKILL,nullptr,0)==0;
            close(fd);if(ok)return true;
        }
    }
    return false;
}
int main(int argc,char** argv){
    if(argc!=7)return 2;
    uint32_t rate=std::getenv("SOUND_RVC_RATE")?std::atoi(std::getenv("SOUND_RVC_RATE")):48000;
    if(!obs_startup("en-US",nullptr,nullptr))return 3;
    obs_audio_info ai{};ai.samples_per_sec=rate;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 4;
    obs_module_t* module=nullptr;if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(module))return 5;
    obs_register_source(&fixture);
    auto* source=obs_source_create("rvc_fixture","tone",nullptr,nullptr);
    auto* d=obs_data_create();obs_data_set_string(d,"helper",argv[3]);obs_data_set_string(d,"library",argv[4]);if(std::getenv("SOUND_RVC_LEGACY"))obs_data_set_string(d,"voice_path","old-voice.onnx");else obs_data_set_string(d,"model",argv[5]);
    if(const char* v=std::getenv("SOUND_RVC_VOICE"))obs_data_set_string(d,"voice_checkpoint",v);
    if(const char* v=std::getenv("SOUND_RVC_INDEX")){obs_data_set_string(d,"retrieval_index",v);obs_data_set_double(d,"retrieval",.3);}
    obs_data_set_double(d,"mix",1);obs_data_set_double(d,"noise",0);
    auto* filter=obs_source_create("stts_rvc_filter","timbre",d,nullptr);
    if(!source||!filter)return 6;
    auto* props=obs_source_properties(filter);
    bool picker=obs_properties_get(props,"pitch")&&obs_properties_get(props,"mix")&&obs_properties_get(props,"model")&&obs_properties_get(props,"voice_checkpoint")&&obs_properties_get(props,"retrieval_index")&&obs_properties_get(props,"retrieval")&&obs_properties_get(props,"packaged_voice")&&obs_properties_get(props,"speaker_id")&&obs_properties_get(props,"rms_mix")&&obs_properties_get(props,"unvoiced_protection")&&obs_properties_get(props,"pitch_median");
    obs_properties_destroy(props);
    obs_source_filter_add(source,filter);obs_set_output_source(1,source);
    obs_source_set_audio_mixers(source,1);obs_source_inc_active(source);obs_source_set_audio_active(source,true);
    obs_source_set_async_unbuffered(source,true);
    Result r;
    for(int i=0;i<4500;++i){r=snapshot(filter);if(r.status=="ready"||r.status=="error")break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    bool missing=std::getenv("SOUND_RVC_MISSING")!=nullptr;
    if(r.status!=(missing?"error":"ready")){printf("RVC init=%s\n",r.status.c_str());return 7;}
    std::ifstream f(argv[6],std::ios::binary);
    std::vector<float> fixture_pcm;float value;
    while(f.read(reinterpret_cast<char*>(&value),sizeof(value)))fixture_pcm.push_back(value);
    if(fixture_pcm.size()<rate)return 8;
    uint64_t timestamp=os_gettime_ns();uint64_t sent=0;uint32_t n=rate/100;
    auto wall=std::chrono::steady_clock::now();
    AudioDispatchTiming timing;
    auto stream=[&](Stats& stats,bool tone,int rounds){
        obs_source_add_audio_capture_callback(source,capture,&stats);
        for(int k=0;k<rounds;++k){
            std::array<float,480> l{},rr{};
            for(size_t i=0;i<n;++i){l[i]=tone?fixture_pcm[(sent+i)%fixture_pcm.size()]:0;rr[i]=l[i]*.5f;}
            obs_source_audio a{};a.data[0]=reinterpret_cast<uint8_t*>(l.data());a.data[1]=reinterpret_cast<uint8_t*>(rr.data());
            a.frames=n;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;a.samples_per_sec=rate;
            a.timestamp=timestamp+sent*1000000000ULL/rate;
            timing.measure(uint64_t(n)*1000000000ULL/rate,[&]{obs_source_output_audio(source,&a);});sent+=n;
            std::this_thread::sleep_until(wall+std::chrono::nanoseconds(sent*1000000000ULL/rate));
        }
        obs_source_remove_audio_capture_callback(source,capture,&stats);
    };
    Stats wet(rate*8),dry(rate*4);
    stream(wet,true,600);
    auto retry_count=[&]{
        calldata_t rd{};proc_handler_call(obs_source_get_proc_handler(filter),"sound_rvc_recovery_v1",&rd);
        auto retries=calldata_int(&rd,"automatic_retries");calldata_free(&rd);return retries;
    };
    auto wet_tail=[&](const Stats& recovered){
        double diff=0,power=0;
        if(recovered.count<rate*2||recovered.invalid)return false;
        for(size_t i=recovered.count-rate*2;i<recovered.count;++i){
            diff+=std::pow(recovered.l[i]-recovered.r[i],2);power+=recovered.l[i]*recovered.l[i];
        }
        return diff<1e-6&&power>1;
    };
    auto finish=[&](bool ok,const char* tag){
        timing.report(tag);
        obs_source_filter_remove(source,filter);obs_source_release(filter);obs_data_release(d);
        obs_set_output_source(1,nullptr);obs_source_dec_active(source);obs_source_release(source);obs_wait_for_destroy_queue();obs_shutdown();
#ifdef SOUND_RT_PROBE
        ok=rt_probe::report(100,{2})&&ok;
#endif
        return ok?0:1;
    };
    if(std::getenv("SOUND_RVC_CRASH_RECOVER")){
        const auto before=snapshot(filter);
        bool killed=crash_owned_helper(argv[3]);
        Stats recovered(rate*20);stream(recovered,true,2000);
        auto last=snapshot(filter);
        bool ok=picker&&killed&&before.status=="ready"&&last.status=="ready"&&
            retry_count()==1&&last.hops>before.hops+4&&wet_tail(recovered);
        printf("RVC_CRASH_RECOVER killed=%d retries=%lld hops=%lld pass=%d\n",killed,(long long)retry_count(),last.hops,ok);
        return finish(ok,"rvc-crash");
    }
    if(std::getenv("SOUND_RVC_IDLE_RECOVER")){
        auto before=snapshot(filter);
        for(int i=0;i<340&&snapshot(filter).status!="idle";++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bool idle=snapshot(filter).status=="idle";
        // Audio timestamps advance with elapsed wall time after a source gap.
        wall=std::chrono::steady_clock::now()-std::chrono::nanoseconds(sent*1000000000ULL/rate);
        timestamp=os_gettime_ns()-sent*1000000000ULL/rate;
        Stats recovered(rate*20);stream(recovered,true,2000);
        auto last=snapshot(filter);
        bool ok=picker&&idle&&last.status=="ready"&&retry_count()==0&&
            last.hops>before.hops+4&&wet_tail(recovered);
        printf("RVC_IDLE_RECOVER idle=%d retries=%lld hops=%lld pass=%d\n",idle,(long long)retry_count(),last.hops,ok);
        return finish(ok,"rvc-idle");
    }
    if(std::getenv("SOUND_RVC_HOT_RELOAD")){
        // The same filter must honor a changed executable path, keep its
        // stereo dry signal, then reload the original real CUDA worker.
        auto before=snapshot(filter);
        obs_data_set_string(d,"helper","/sound-rvc-test-missing-worker");
        obs_source_update(filter,d);stream(dry,true,400);
        auto bad=snapshot(filter);
        bool fallback=bad.status=="error"&&rms(dry,rate,true)>.005&&!dry.invalid;
        obs_data_set_string(d,"helper",argv[3]);obs_source_update(filter,d);
        Stats recovered(rate*25);stream(recovered,true,2500);
        auto last=snapshot(filter);
        double diff=0,power=0;
        if(recovered.count>=rate*2)for(size_t i=recovered.count-rate*2;i<recovered.count;++i){
            diff+=std::pow(recovered.l[i]-recovered.r[i],2);power+=recovered.l[i]*recovered.l[i];
        }
        bool resumed=last.status=="ready"&&last.hops>before.hops+4&&recovered.count>=rate*2&&
            diff<1e-6&&power>1&&!recovered.invalid;
        bool ok=picker&&before.status=="ready"&&fallback&&resumed;
        printf("RVC_HOT_RELOAD fallback=%d recovered=%d hops=%lld pass=%d\n",fallback,resumed,last.hops,ok);
        return finish(ok,"rvc-reload");
    }
    obs_data_set_double(d,"rms_mix",.8);obs_data_set_double(d,"unvoiced_protection",.1);obs_data_set_bool(d,"pitch_median",false);
    obs_data_set_double(d,"mix",0);obs_source_update(filter,d);
    stream(dry,true,300);
    auto last=snapshot(filter);
    bool ok=picker&&!wet.invalid&&!dry.invalid&&wet.count>rate*5&&dry.count>rate*2;
    double wet_stereo=rms(wet,rate,true),dry_stereo=rms(dry,rate,true);
    // Ignore the initial 1.5 s latency and 10 ms mix ramp.
    if(missing)ok=ok&&last.status=="error"&&last.hops==0&&wet_stereo>.005;
    else ok=ok&&last.status=="ready"&&last.hops>=16&&wet_stereo<.005&&dry_stereo>.005&&rms(wet,rate)>.005;
    printf("RVC_OBS rate=%u status=%s hops=%lld last_ms=%.1f wet_rms=%.5f wet_stereo=%.5f dry_stereo=%.5f pass=%d\n",rate,last.status.c_str(),last.hops,last.ms,rms(wet,rate),wet_stereo,dry_stereo,ok);
    return finish(ok,"rvc-native");
}
