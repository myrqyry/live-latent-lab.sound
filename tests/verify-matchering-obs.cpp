#ifdef SOUND_RT_PROBE
#include "rt-callback-probe.hpp"
#endif
// Real libobs source/filter/capture path using a generated WAV reference.
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>
#include <chrono>

constexpr size_t N=480;
struct Capture {
    std::array<float,N> l{},r{};
    size_t calls=0;
};
static void capture(void* opaque,obs_source_t*,const audio_data* p,bool){
    auto& c=*static_cast<Capture*>(opaque);
    if(p->frames!=N||!p->data[0]||!p->data[1])return;
    const auto* l=reinterpret_cast<const float*>(p->data[0]);
    const auto* r=reinterpret_cast<const float*>(p->data[1]);
    std::copy_n(l,N,c.l.begin());std::copy_n(r,N,c.r.begin());++c.calls;
}
static const char* name(void*){return "Matchering test input";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info fixture={
    .id="sound_matchering_test_input",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,
    .get_name=name,.create=create,.destroy=destroy
};
int main(int argc,char** argv){
    assert(argc==4);
    assert(obs_startup("en-US",nullptr,nullptr));
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    assert(obs_reset_audio(&ai));
    obs_module_t* module=nullptr;
    assert(obs_open_module(&module,argv[1],argv[2])==MODULE_SUCCESS && obs_init_module(module));
    obs_register_source(&fixture);
    auto* input=obs_source_create(fixture.id,"Matchering test",nullptr,nullptr);
    auto* settings=obs_data_create();
    obs_data_set_string(settings,"reference_path",argv[3]);
    obs_data_set_double(settings,"mix",0.0);
    auto* filter=obs_source_create("matchering_filter","Test Matchering",settings,nullptr);
    assert(input&&filter);
    obs_source_filter_add(input,filter);
    obs_source_set_audio_active(input,true);
    obs_source_set_async_unbuffered(input,true);
    Capture got;
    obs_source_add_audio_capture_callback(input,capture,&got);
    std::array<float,N> l{},r{};
    for(size_t i=0;i<N;++i){l[i]=.2f*std::sin(float(i)*.15f);r[i]=.13f*std::cos(float(i)*.09f);}
    uint64_t timestamp=os_gettime_ns();
    auto feed=[&]{
        auto before=got.calls;
        obs_source_audio audio{};
        audio.data[0]=reinterpret_cast<uint8_t*>(l.data());audio.data[1]=reinterpret_cast<uint8_t*>(r.data());
        audio.frames=N;audio.speakers=SPEAKERS_STEREO;audio.format=AUDIO_FORMAT_FLOAT_PLANAR;
        audio.samples_per_sec=48000;audio.timestamp=timestamp;timestamp+=10000000;
        obs_source_output_audio(input,&audio);
        assert(got.calls==before+1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    feed();assert(got.l==l && got.r==r);
    obs_data_set_double(settings,"mix",1.0);obs_source_update(filter,settings);
    feed();
    assert(got.l!=l && got.r!=r);
#ifdef SOUND_RT_PROBE
    for(int i=0;i<200;++i){feed();for(float v:got.l)assert(std::isfinite(v));}
#endif
    for(size_t i=0;i<N;++i)assert(std::isfinite(got.l[i]) && std::isfinite(got.r[i]));
    obs_data_set_string(settings,"reference_path","");obs_source_update(filter,settings);
    feed();assert(got.l==l && got.r==r);
    obs_source_remove_audio_capture_callback(input,capture,&got);
    obs_source_filter_remove(input,filter);
    obs_source_release(filter);obs_source_release(input);obs_data_release(settings);obs_shutdown();
    puts("MATCHERING_OBS_REAL_AUDIO_PASS=1");
#ifdef SOUND_RT_PROBE
    if(!rt_probe::report(203,{5}))return 1;
#endif
}
