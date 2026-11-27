// Real libobs + ONNX Runtime + deterministic synthetic gain graphs.
// Verifies transport and model switch behavior, not noise suppression quality.
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <thread>

constexpr size_t N=480;
struct Capture { std::array<float,N> l{},r{}; size_t calls=0; };
static void capture(void* opaque,obs_source_t*,const audio_data* p,bool){
    auto& c=*static_cast<Capture*>(opaque);
    if(p->frames!=N||!p->data[0]||!p->data[1])return;
    std::copy_n(reinterpret_cast<const float*>(p->data[0]),N,c.l.begin());
    std::copy_n(reinterpret_cast<const float*>(p->data[1]),N,c.r.begin());
    ++c.calls;
}
static const char* name(void*){return "DeepFilter transport test input";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info fixture={
    .id="sound_df_transport_test",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,
    .get_name=name,.create=create,.destroy=destroy
};
int main(int argc,char** argv){
    assert(argc==5);
    assert(obs_startup("en-US",nullptr,nullptr));
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    assert(obs_reset_audio(&ai));
    obs_module_t* module=nullptr;
    assert(obs_open_module(&module,argv[1],argv[2])==MODULE_SUCCESS && obs_init_module(module));
    obs_register_source(&fixture);
    auto* input=obs_source_create(fixture.id,"DF test",nullptr,nullptr);
    auto* settings=obs_data_create();
    obs_data_set_bool(settings,"passthrough_on_underrun",false);
    auto* filter=obs_source_create("deepfilter_noise_suppression","DF filter",settings,nullptr);
    assert(input&&filter);
    obs_source_filter_add(input,filter);
    obs_source_set_audio_active(input,true);
    obs_source_set_async_unbuffered(input,true);
    Capture got;
    obs_source_add_audio_capture_callback(input,capture,&got);
    std::array<float,N> l{},r{};l.fill(.4f);r.fill(-.2f);
    uint64_t timestamp=os_gettime_ns();
    auto feed=[&]{
        auto before=got.calls;
        obs_source_audio a{};
        a.data[0]=reinterpret_cast<uint8_t*>(l.data());a.data[1]=reinterpret_cast<uint8_t*>(r.data());
        a.frames=N;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;
        a.samples_per_sec=48000;a.timestamp=timestamp;timestamp+=10000000;
        obs_source_output_audio(input,&a);
        assert(got.calls==before+1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };
    auto all=[&](float a,float b){
        for(size_t i=0;i<N;++i)
            if(std::abs(got.l[i]-a)>1e-6f || std::abs(got.r[i]-b)>1e-6f)return false;
        return true;
    };
    feed();assert(all(.4f,-.2f)); // no model never mutes source
    auto set_model=[&](const char* path){
        obs_data_set_string(settings,"model_path",path);
        obs_source_update(filter,settings);
    };
    auto expect_wet=[&](float expected){
        bool wet=false;
        for(int i=0;i<40;++i){
            feed();
            assert(all(0.f,0.f)||all(expected,expected)); // no previous generation output
            wet|=all(expected,expected);
        }
        assert(wet);
    };
    set_model(argv[3]);expect_wet(.1f);
    set_model(argv[4]);expect_wet(.3f);
    set_model("");feed();assert(all(.4f,-.2f));
    obs_source_remove_audio_capture_callback(input,capture,&got);
    obs_source_filter_remove(input,filter);
    obs_source_release(filter);obs_source_release(input);obs_data_release(settings);obs_shutdown();
    puts("DEEPFILTER_OBS_REAL_ORT_TRANSPORT_PASS=1");
}
