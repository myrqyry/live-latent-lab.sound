// Actual OBS audio routing test: load the built module, instantiate its
// production Mic filter, attach it to a synthetic stereo source, and capture
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
    if(!obs_reset_audio(&ai))return 4;
    obs_module_t* module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS || !obs_init_module(module))return 5;
    obs_register_source(&fixture_source);
    auto* source=obs_source_create("test_mic_audio_generator","tone",nullptr,nullptr);
    auto* settings=obs_data_create();
    obs_data_set_int(settings,"mic_type",0);
    auto* filter=obs_source_create("mic_modeler_filter","mic-test",settings,nullptr);
    if(!source||!filter)return 6;
    obs_properties_t* props=obs_get_source_properties("mic_modeler_filter");
    obs_property_t* preset=props ? obs_properties_get(props,"mic_type") : nullptr;
    const bool properties_ok=props && preset &&
        obs_property_list_item_count(preset)==7 &&
        !obs_properties_get(props,"model_path") &&
        !obs_properties_get(props,"backend");
    if(props) obs_properties_destroy(props);
    obs_source_filter_add(source,filter);
    obs_set_output_source(1,source);
    obs_source_set_audio_mixers(source,1);
    obs_source_inc_active(source);
    obs_source_set_audio_active(source, true);
    obs_source_set_async_unbuffered(source, true);
    Stats off;
    obs_source_add_audio_capture_callback(source,capture,&off);
    stream(source,60);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    obs_source_remove_audio_capture_callback(source,capture,&off);
    obs_data_set_int(settings,"mic_type",3); // Ribbon high-end rolloff
    obs_source_update(filter,settings);
    Stats on;
    obs_source_add_audio_capture_callback(source,capture,&on);
    stream(source,60);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    obs_source_remove_audio_capture_callback(source,capture,&on);
    // Legacy scenes may still persist mic_type=7, the quarantined neural mode.
    // Changing to that option must not accidentally process or mute audio.
    obs_data_set_int(settings,"mic_type",7);
    obs_source_update(filter,settings);
    Stats legacy;
    obs_source_add_audio_capture_callback(source,capture,&legacy);
    stream(source,60);
    obs_source_remove_audio_capture_callback(source,capture,&legacy);
    double off_l=std::sqrt(off.sum_l.load()/std::max<uint64_t>(1,off.samples.load()));
    double on_l=std::sqrt(on.sum_l.load()/std::max<uint64_t>(1,on.samples.load()));
    double off_r=std::sqrt(off.sum_r.load()/std::max<uint64_t>(1,off.samples.load()));
    double on_r=std::sqrt(on.sum_r.load()/std::max<uint64_t>(1,on.samples.load()));
    double legacy_l=std::sqrt(legacy.sum_l.load()/std::max<uint64_t>(1,legacy.samples.load()));
    double legacy_r=std::sqrt(legacy.sum_r.load()/std::max<uint64_t>(1,legacy.samples.load()));
    bool passed=properties_ok && off.samples.load()>4800 && on.samples.load()>4800 &&
                legacy.samples.load()>4800 &&
                off_l>.005 && off_r>.001 && on_l/off_l<.95 && on_r/off_r<.95 &&
                std::abs(legacy_l/off_l-1.)<.03 && std::abs(legacy_r/off_r-1.)<.03 &&
                std::abs((on_r/on_l)/(off_r/off_l)-1.)<.08;
    std::printf("MIC_OBS_ACTUAL_AUDIO off=%g,%g ribbon=%g,%g legacy_onnx_dry=%g,%g properties=%d frames=%llu,%llu,%llu pass=%d\n",
                off_l,off_r,on_l,on_r,legacy_l,legacy_r,properties_ok,
                (unsigned long long)off.samples.load(),
                (unsigned long long)on.samples.load(),
                (unsigned long long)legacy.samples.load(),passed);
    obs_set_output_source(1,nullptr);
    obs_source_dec_active(source);
    obs_source_filter_remove(source,filter);
    obs_source_release(filter);
    obs_source_release(source);
    obs_data_release(settings);
    obs_shutdown();
    return passed?0:1;
}
