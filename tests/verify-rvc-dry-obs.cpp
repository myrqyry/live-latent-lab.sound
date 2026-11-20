// RVC fail-dry test: real libobs source audio with an unconfigured or invalid
// experimental RVC filter; no fake model or fake callback
// source output after real filter execution. No fake filter callbacks.
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <array>

struct Stats {
    std::atomic<uint64_t> samples{0};
    std::atomic<double> sum_l{0}, sum_r{0};
};
static void add(std::atomic<double>& val, double x) {
    double old = val.load();
    while (!val.compare_exchange_weak(old, old+x)) {}
}
static void capture(void* param, obs_source_t*, const struct audio_data* data, bool) {
    auto& s = *static_cast<Stats*>(param);
    auto* l = reinterpret_cast<float*>(data->data[0]);
    auto* r = reinterpret_cast<float*>(data->data[1]);
    if (!l || !r) return;
    double sl = 0, sr = 0;
    for (unsigned i=0;i<data->frames;++i) {
        sl += l[i]*l[i];
        sr += r[i]*r[i];
    }
    add(s.sum_l, sl); add(s.sum_r, sr);
    s.samples += data->frames;
}
static const char* fixture_name(void*) { return "Verification stereo generator"; }
static void* fixture_create(obs_data_t*,obs_source_t*) { return reinterpret_cast<void*>(1); }
static void fixture_destroy(void*) {}
static obs_source_info fixture_source = {
    .id="test_mic_audio_generator",
    .type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=fixture_name,
    .create=fixture_create,
    .destroy=fixture_destroy,
};
static void stream(obs_source_t* source, int rounds) {
    static constexpr uint32_t FRAMES=480;
    std::array<float, FRAMES> l{},r{};
    uint64_t start=os_gettime_ns();
    for (int k=0;k<rounds;++k) {
        for (unsigned i=0;i<FRAMES;++i) {
            const float phase=float((uint64_t(k)*FRAMES+i)*6.283185307179586*9000.0/48000.0);
            l[i]=.4f*std::sin(phase);
            r[i]=.16f*std::sin(phase);
        }
        obs_source_audio audio{};
        audio.data[0]=reinterpret_cast<uint8_t*>(l.data());
        audio.data[1]=reinterpret_cast<uint8_t*>(r.data());
        audio.frames=FRAMES;
        audio.speakers=SPEAKERS_STEREO;
        audio.format=AUDIO_FORMAT_FLOAT_PLANAR;
        audio.samples_per_sec=48000;
        audio.timestamp=start+uint64_t(k)*10000000ULL;
        obs_source_output_audio(source,&audio);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
int main(int argc,char**argv) {
    if(argc!=3)return 2;
    if(!obs_startup("en-US",nullptr,nullptr))return 3;
    obs_audio_info ai{};
    ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai)) { obs_shutdown(); return 4; }
    obs_module_t* module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS ||
       !obs_init_module(module)) { obs_shutdown(); return 5; }
    obs_register_source(&fixture_source);
    auto* source=obs_source_create("test_mic_audio_generator","rvc-source",nullptr,nullptr);
    auto* settings=obs_data_create();
    auto* filter=obs_source_create("stts_rvc_filter","rvc-test",settings,nullptr);
    if(!source||!filter) { obs_shutdown(); return 6; }
    obs_source_filter_add(source,filter);
    obs_set_output_source(1,source);
    obs_source_set_audio_mixers(source,1);
    obs_source_inc_active(source);
    obs_source_set_audio_active(source,true);

    obs_properties_t* props=obs_get_source_properties("stts_rvc_filter");
    const bool property_ok=props && obs_properties_get(props,"voice_sample_rate");
    if(props)obs_properties_destroy(props);
    Stats unconfigured;
    obs_source_add_audio_capture_callback(source,capture,&unconfigured);
    stream(source,60);
    obs_source_remove_audio_capture_callback(source,capture,&unconfigured);

    // Real ORT must reject these unreadable files; false readiness cannot
    // feed silence or collapse stereo. This executes the production update.
    obs_data_set_string(settings,"voice_path","/dev/null");
    obs_data_set_string(settings,"pitch_path","/dev/null");
    obs_data_set_string(settings,"content_path","/dev/null");
    obs_data_set_int(settings,"voice_sample_rate",40000);
    obs_source_update(filter,settings);
    Stats rejected;
    obs_source_add_audio_capture_callback(source,capture,&rejected);
    stream(source,60);
    obs_source_remove_audio_capture_callback(source,capture,&rejected);

    const auto rms=[](const Stats& st,bool right) {
        return std::sqrt((right?st.sum_r.load():st.sum_l.load()) /
            double(std::max<uint64_t>(1,st.samples.load())));
    };
    const double a=rms(unconfigured,false), b=rms(unconfigured,true);
    const double c=rms(rejected,false), d=rms(rejected,true);
    const bool pass=property_ok && unconfigured.samples.load()==60*480 &&
        rejected.samples.load()==60*480 && a>.05 && b>.01 &&
        std::abs(c/a-1.)<.001 && std::abs(d/b-1.)<.001 &&
        std::abs(b/a-.4)<.001;
    std::printf("RVC_DRY_SOURCE_CAPTURE default=%g,%g invalid_models=%g,%g properties=%d frames=%llu,%llu pass=%d\n",
        a,b,c,d,property_ok,
        (unsigned long long)unconfigured.samples.load(),
        (unsigned long long)rejected.samples.load(),pass);
    obs_set_output_source(1,nullptr);
    obs_source_dec_active(source);
    obs_source_filter_remove(source,filter);
    obs_source_release(filter);obs_source_release(source);
    obs_data_release(settings);
    obs_shutdown();
    return pass?0:1;
}
