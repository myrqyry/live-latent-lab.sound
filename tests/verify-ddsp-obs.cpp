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
static const char* name(void*){return "DDSP fixture";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info fixture={.id="ddsp_fixture",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=create,.destroy=destroy};
struct Result{std::string status;long long hops=0;double pitch=0,ms=0;};
static Result snapshot(obs_source_t* f){
    calldata_t d{};Result r;
    if(proc_handler_call(obs_source_get_proc_handler(f),"sound_ddsp_status_v1",&d)){
        const char* s=calldata_string(&d,"status");if(s)r.status=s;
        r.hops=calldata_int(&d,"hops");r.pitch=calldata_float(&d,"pitch_hz");r.ms=calldata_float(&d,"inference_ms");
    }
    calldata_free(&d);return r;
}
static double magnitude(const Stats& s,size_t hz,double frequency){
    size_t start=hz/2,n=s.count-start;if(s.count<=start)return 0;
    double a=0,b=0;
    for(size_t i=start;i<s.count;++i){double phase=6.283185307179586*frequency*i/hz;a+=s.l[i]*std::cos(phase);b+=s.l[i]*std::sin(phase);}
    return 2*std::hypot(a,b)/n;
}
static double rms(const Stats& s,size_t rate,bool stereo_difference=false){
    if(s.count<=rate/2)return 0;double power=0;
    for(size_t i=rate/2;i<s.count;++i){double v=stereo_difference?s.l[i]-s.r[i]:s.l[i];power+=v*v;}
    return std::sqrt(power/(s.count-rate/2));
}
int main(int argc,char** argv){
    if(argc!=5)return 2;
    uint32_t rate=std::getenv("SOUND_DDSP_RATE")?std::atoi(std::getenv("SOUND_DDSP_RATE")):48000;
    if(!obs_startup("en-US",nullptr,nullptr))return 3;
    obs_audio_info ai{};ai.samples_per_sec=rate;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 4;
    obs_module_t* module=nullptr;if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(module))return 5;
    obs_register_source(&fixture);
    auto* source=obs_source_create("ddsp_fixture","tone",nullptr,nullptr);
    auto* d=obs_data_create();obs_data_set_string(d,"runtime",argv[3]);obs_data_set_string(d,"models",argv[4]);
    obs_data_set_double(d,"mix",1);obs_data_set_double(d,"noise",0);
    auto* filter=obs_source_create("sound_ddsp_filter","timbre",d,nullptr);
    if(!source||!filter)return 6;
    auto* props=obs_source_properties(filter);
    auto* list=obs_properties_get(props,"instrument_a");
    bool picker=list&&obs_property_list_item_count(list)==11;
    obs_properties_destroy(props);
    obs_source_filter_add(source,filter);obs_set_output_source(1,source);
    obs_source_set_audio_mixers(source,1);obs_source_inc_active(source);obs_source_set_audio_active(source,true);
    obs_source_set_async_unbuffered(source,true);
    Result r;
    for(int i=0;i<300;++i){r=snapshot(filter);if(r.status=="ready"||r.status=="error")break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    bool missing=std::getenv("SOUND_DDSP_MISSING")!=nullptr;
    if(r.status!=(missing?"error":"ready")){printf("DDSP init=%s\n",r.status.c_str());return 7;}
    uint64_t timestamp=os_gettime_ns();uint64_t sent=0;uint32_t n=rate/100;
    auto wall=std::chrono::steady_clock::now();
    auto stream=[&](Stats& stats,bool tone,int rounds){
        obs_source_add_audio_capture_callback(source,capture,&stats);
        for(int k=0;k<rounds;++k){
            std::array<float,480> l{},rr{};
            for(size_t i=0;i<n;++i){l[i]=tone?float(.3*std::sin(6.28318530718*220*(sent+i)/rate)):0;rr[i]=l[i]*.5f;}
            obs_source_audio a{};a.data[0]=reinterpret_cast<uint8_t*>(l.data());a.data[1]=reinterpret_cast<uint8_t*>(rr.data());
            a.frames=n;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;a.samples_per_sec=rate;
            a.timestamp=timestamp+sent*1000000000ULL/rate;
            obs_source_output_audio(source,&a);sent+=n;
            std::this_thread::sleep_until(wall+std::chrono::nanoseconds(sent*1000000000ULL/rate));
        }
        obs_source_remove_audio_capture_callback(source,capture,&stats);
    };
    Stats a(rate*2),b(rate*2),switched(rate*2),shift(rate*2),noise(rate*2),dry(rate*2),silence(rate*2);
    stream(a,true,170);auto result=snapshot(filter);
    bool ok=picker&&a.count>rate&&!a.invalid;
    if(missing){
        ok=ok&&result.hops==0&&rms(a,rate,true)>.08;
        if(const char* recovered_models=std::getenv("SOUND_DDSP_RECOVER_MODELS")){
            obs_data_set_string(d,"models",recovered_models);obs_source_update(filter,d);
            for(int i=0;i<500;++i){r=snapshot(filter);if(r.status=="ready")break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            // Start the second audio stream from a fresh monotonic origin.
            wall=std::chrono::steady_clock::now()-std::chrono::nanoseconds(sent*1000000000ULL/rate);
            timestamp=os_gettime_ns()-sent*1000000000ULL/rate;
            stream(b,true,170);
            auto recovery=snapshot(filter);
            bool recovered=r.status=="ready"&&recovery.status=="ready"&&recovery.hops>65&&rms(b,rate)>.005&&rms(b,rate,true)<1e-5;
            printf("DDSP_RECOVER_MODELS=%d hops=%lld\n",recovered,recovery.hops);
            ok=ok&&recovered;
        }
        printf("DDSP_MISSING dry_rms=%.5f stereo_difference=%.5f pass=%d\n",rms(a,rate),rms(a,rate,true),ok);
    }else{
        obs_data_set_double(d,"morph",1);obs_source_update(filter,d);stream(b,true,170);
        obs_data_set_string(d,"instrument_b","Trumpet");obs_source_update(filter,d);stream(switched,true,170);
        obs_data_set_string(d,"instrument_b","Flute");obs_data_set_double(d,"pitch",12);obs_source_update(filter,d);stream(shift,true,170);
        obs_data_set_double(d,"harmonics",0);obs_data_set_double(d,"noise",2);obs_data_set_double(d,"pitch",0);
        obs_source_update(filter,d);stream(noise,true,170);
        obs_data_set_double(d,"harmonics",1);obs_data_set_double(d,"noise",1);
        obs_data_set_double(d,"mix",0);obs_source_update(filter,d);stream(dry,true,170);
        obs_data_set_double(d,"mix",1);obs_source_update(filter,d);stream(silence,false,170);
        auto last=snapshot(filter);
        double shape=0;for(int k=1;k<=12;++k)shape+=std::abs(magnitude(a,rate,k*220)-magnitude(b,rate,k*220));
        double switch_shape=0;for(int k=1;k<=12;++k)switch_shape+=std::abs(magnitude(switched,rate,k*220)-magnitude(b,rate,k*220));
        double a_rms=rms(a,rate),b_rms=rms(b,rate),dry_rms=rms(dry,rate),silent_rms=rms(silence,rate);
        double shift440=magnitude(shift,rate,440),shift220=magnitude(shift,rate,220);
        ok=ok&&last.status=="ready"&&last.hops>430&&std::abs(result.pitch-220)<5
           &&!b.invalid&&!switched.invalid&&!shift.invalid&&!dry.invalid&&!silence.invalid
           &&a_rms>.005&&b_rms>.005&&shape>.015&&switch_shape>.015&&shift440>shift220*2&&shift440>.005
           &&rms(noise,rate)>.0001&&!noise.invalid&&rms(a,rate,true)<1e-5&&rms(dry,rate,true)>.08&&std::abs(dry_rms-.3/std::sqrt(2.))<.01&&silent_rms<.001;
        printf("DDSP_CHECK picker=%d count=%zu status=%s invalid=%d%d%d%d%d stereo_wet=%.8f stereo_dry=%.8f\n",picker,a.count,last.status.c_str(),a.invalid,b.invalid,shift.invalid,dry.invalid,silence.invalid,rms(a,rate,true),rms(dry,rate,true));
        printf("DDSP_OBS rate=%u hops=%lld pitch=%.3f last_ms=%.3f a_rms=%.5f b_rms=%.5f morph_distance=%.5f switch_distance=%.5f shifted440=%.5f shifted220=%.5f noise_rms=%.6f dry_rms=%.5f silence=%.7f pass=%d\n",
           rate,last.hops,result.pitch,last.ms,a_rms,b_rms,shape,switch_shape,shift440,shift220,rms(noise,rate),dry_rms,silent_rms,ok);
    }
    obs_source_filter_remove(source,filter);obs_source_release(filter);obs_data_release(d);
    obs_set_output_source(1,nullptr);obs_source_dec_active(source);obs_source_release(source);obs_wait_for_destroy_queue();obs_shutdown();
    return ok?0:1;
}
