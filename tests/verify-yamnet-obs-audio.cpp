#ifdef SOUND_RT_PROBE
#include "rt-callback-probe.hpp"
#endif
// Real libobs source audio through the production opt-in native YAMNet
// filter. Model must classify actual supplied silence and sine audio.
// The source audio must pass through unchanged; no mocked classifier.
#include <obs.h>
#include <obs-module.h>
#include <callback/proc.h>
#include <util/platform.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
struct Stats {
    std::atomic<uint64_t> frames{0};
    std::atomic<double> l2{0},r2{0};
};
static void add(std::atomic<double>& v,double change){
    double old=v.load();
    while(!v.compare_exchange_weak(old,old+change)){}
}
static void capture(void* arg,obs_source_t*,const audio_data* p,bool) {
    auto& s=*static_cast<Stats*>(arg);
    if(!p->data[0]||!p->data[1])return;
    auto* left=reinterpret_cast<const float*>(p->data[0]);
    auto* right=reinterpret_cast<const float*>(p->data[1]);
    double l=0,r=0;
    for(size_t i=0;i<p->frames;++i){l+=left[i]*left[i];r+=right[i]*right[i];}
    add(s.l2,l);add(s.r2,r);s.frames.fetch_add(p->frames);
}
static const char* name(void*){return "Sound YAMNet stereo test source";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info fixture={
    .id="sound_yamnet_test_source",
    .type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=name,
    .create=create,
    .destroy=destroy,
};
struct Result {
    std::string label,status;
    double score=0;
    long long windows=0,dropped=0,inference_ms=0,callback_calls=0;
    bool available=false;
};
static Result snapshot(obs_source_t* filter) {
    Result out;
    auto* handler=obs_source_get_proc_handler(filter);
    calldata_t data{};
    if(handler && proc_handler_call(handler,"sound_yamnet_read_v1",&data)){
        out.available=calldata_bool(&data,"available");
        out.windows=calldata_int(&data,"windows");
        out.dropped=calldata_int(&data,"dropped_samples");
        out.inference_ms=calldata_int(&data,"inference_ms");
        out.callback_calls=calldata_int(&data,"callback_calls");
        out.score=calldata_float(&data,"score");
        const char* label=calldata_string(&data,"label");
        const char* status=calldata_string(&data,"status");
        if(label)out.label=label;
        if(status)out.status=status;
    }
    calldata_free(&data);
    return out;
}
static void stream(obs_source_t* source,int rounds,bool sine,uint32_t hz,
                   Stats* stats=nullptr) {
    const uint32_t frames=hz/100;
    std::array<float,480> l{},r{};
    uint64_t start=os_gettime_ns();
    if(stats)obs_source_add_audio_capture_callback(source,capture,stats);
    for(int k=0;k<rounds;++k) {
        for(uint32_t i=0;i<frames;++i) {
            const float v=sine?float(.30*std::sin(
                6.28318530717958647692*440.0*(double(k)*frames+i)/hz)):0.f;
            l[i]=v;r[i]=v*.5f;
        }
        obs_source_audio audio{};
        audio.data[0]=reinterpret_cast<uint8_t*>(l.data());
        audio.data[1]=reinterpret_cast<uint8_t*>(r.data());
        audio.frames=frames;
        audio.speakers=SPEAKERS_STEREO;
        audio.format=AUDIO_FORMAT_FLOAT_PLANAR;
        audio.samples_per_sec=hz;
        audio.timestamp=start+uint64_t(k)*10000000ULL;
        obs_source_output_audio(source,&audio);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(stats)obs_source_remove_audio_capture_callback(source,capture,stats);
}
static Result await(obs_source_t* f,long long min_windows,int ms=3200) {
    Result out;
    for(int i=0;i<ms/25;++i) {
        out=snapshot(f);
        if(out.available && out.windows>=min_windows && out.status=="ready")break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return out;
}
int main(int argc,char** argv){
    if(argc!=5) {
        std::fprintf(stderr,"usage: test MODULE STAGE_DATA LIB MODEL\n");
        return 2;
    }
    if(!obs_startup("en-US",nullptr,nullptr))return 3;
    const char* rate_env=std::getenv("SOUND_YAMNET_TEST_RATE");
    const uint32_t hz=rate_env?uint32_t(std::atoi(rate_env)):48000;
    if(hz!=44100 && hz!=48000)return 10;
    obs_audio_info ai{};
    ai.samples_per_sec=hz;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 4;
    obs_module_t* module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS ||
       !obs_init_module(module))return 5;
    obs_register_source(&fixture);
    auto* source=obs_source_create("sound_yamnet_test_source","yamnet-input",nullptr,nullptr);
    auto* settings=obs_data_create();
    obs_data_set_string(settings,"mediapipe_library",argv[3]);
    obs_data_set_string(settings,"yamnet_model",argv[4]);
    const std::string device=std::getenv("SOUND_YAMNET_DEVICE")?std::getenv("SOUND_YAMNET_DEVICE"):"cpu";
    obs_data_set_string(settings,"yamnet_device",device.c_str());
    auto* filter=obs_source_create("yamnet_classifier_source","YAMNet-worker",settings,nullptr);
    if(!source||!filter)return 6;
    obs_properties_t* properties=obs_get_source_properties("yamnet_classifier_source");
    const bool props=properties &&
        obs_properties_get(properties,"mediapipe_library") &&
        obs_properties_get(properties,"yamnet_model")&&obs_properties_get(properties,"yamnet_device");
    if(properties)obs_properties_destroy(properties);
    obs_source_filter_add(source,filter);
    obs_set_output_source(1,source);
    obs_source_set_audio_mixers(source,1);
    obs_source_inc_active(source);
    obs_source_set_audio_active(source,true);
    obs_source_set_async_unbuffered(source,true);
    // Wait for actual native model initialization; model warm-up duration
    // is not deterministic under a debugger or sustained OBS load.
    Result ready;
    for(int i=0;i<200;++i) {
        ready=snapshot(filter);
        if(ready.status=="ready")break;
        if(ready.status=="error"||ready.status=="overload")break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    std::printf("SOUND_YAMNET_WORKER_READY=%d status=%s\n",
                int(ready.status=="ready"),ready.status.c_str());
    calldata_t backend{};
    bool backend_ok=proc_handler_call(obs_source_get_proc_handler(filter),"sound_yamnet_device_v1",&backend);
    const char* requested=calldata_string(&backend,"requested");const char* selected=calldata_string(&backend,"selected");
    backend_ok=backend_ok&&requested&&device==requested&&selected&&std::string(selected)=="cpu";
    printf("SOUND_YAMNET_DEVICE requested=%s selected=%s pass=%d\n",requested?requested:"",selected?selected:"",backend_ok);
    calldata_free(&backend);
    if(std::getenv("SOUND_YAMNET_EXPECT_GPU_FAILURE")){
        Stats dry;
        stream(source,35,true,hz,&dry);
        bool ok=device=="gpu"&&ready.status=="error"&&!ready.available&&ready.windows==0&&dry.frames>=hz/5&&
            dry.l2.load()>0.001&&dry.r2.load()>0.001;
        printf("SOUND_YAMNET_EXPLICIT_GPU_FAILURE dry_frames=%llu pass=%d\n",(unsigned long long)dry.frames.load(),ok);
        obs_source_filter_remove(source,filter);obs_source_release(filter);obs_data_release(settings);
        obs_set_output_source(1,nullptr);obs_source_dec_active(source);obs_source_release(source);obs_wait_for_destroy_queue();obs_shutdown();
        return ok?0:1;
    }
    if(ready.status!="ready"||!backend_ok)return 7;
    Stats silence_audio;
    stream(source,115,false,hz,&silence_audio);
    Result silence=await(filter,1);
    std::printf("SOUND_YAMNET_REAL_OBS_SILENCE status=%s label=%s score=%g windows=%lld dropped=%lld\n",
        silence.status.c_str(),silence.label.c_str(),silence.score,
        silence.windows,silence.dropped);
    Stats tone_audio;
    stream(source,235,true,hz,&tone_audio);
    Result tone=await(filter,3);
    std::printf("SOUND_YAMNET_REAL_OBS_TONE status=%s label=%s score=%g windows=%lld dropped=%lld inference_ms=%lld audio_frames=%llu\n",
        tone.status.c_str(),tone.label.c_str(),tone.score,
        tone.windows,tone.dropped,tone.inference_ms,
        (unsigned long long)tone_audio.frames.load());
    // Discover actual model classes, not a hardcoded top-class name.
    auto* inventory=obs_data_create();
    calldata_t list{};calldata_set_ptr(&list,"inventory",inventory);
    const bool listed=proc_handler_call(obs_source_get_proc_handler(filter),"sound_yamnet_list_v1",&list);
    calldata_free(&list);
    auto* rows=obs_data_get_array(inventory,"categories");
    const size_t categories=rows?obs_data_array_count(rows):0;
    bool sine_class=false,silence_class=false;
    for(size_t i=0;i<categories;++i){
        auto* row=obs_data_array_item(rows,i);
        const std::string label=obs_data_get_string(row,"label");
        sine_class|=label=="Sine wave";silence_class|=label=="Silence";
        obs_data_release(row);
    }
    if(rows)obs_data_array_release(rows);
    obs_data_release(inventory);
    auto score=[&](const char* label,double& value){
        calldata_t call{};calldata_set_string(&call,"category",label);
        const bool available=proc_handler_call(obs_source_get_proc_handler(filter),"sound_yamnet_score_v1",&call)
            &&calldata_bool(&call,"available");
        value=calldata_float(&call,"score");
        calldata_free(&call);return available;
    };
    double sine_score=0,silence_score=0,missing_score=0;
    const bool class_scores=score("Sine wave",sine_score)&&score("Silence",silence_score)&&
        !score("not an actual class",missing_score)&&sine_score>.6&&silence_score<.3;
    const bool catalog=listed&&categories==521&&sine_class&&silence_class;
    std::printf("SOUND_YAMNET_CATEGORY_SIGNAL catalog=%d count=%zu scores=%d sine=%g silence=%g\n",
        int(catalog),categories,int(class_scores),sine_score,silence_score);
    // Failure path through the actual OBS filter: bad model cannot
    // classify or silence/bend the original microphone signal.
    auto* invalid_settings=obs_data_create();
    obs_data_set_string(invalid_settings,"mediapipe_library",argv[3]);
    obs_data_set_string(invalid_settings,"yamnet_model",
                        "/tmp/sound-assert-model-absent-does-not-exist.tflite");
    auto* bad=obs_source_create("yamnet_classifier_source",
                                 "YAMNet-missing-model",invalid_settings,nullptr);
    if(!bad)return 11;
    obs_source_filter_add(source,bad);
    Result failure;
    for(int i=0;i<120;++i){
        failure=snapshot(bad);
        if(failure.status=="error")break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    Stats dry_audio;
    stream(source,32,true,hz,&dry_audio);
    obs_source_filter_remove(source,bad);
    obs_source_release(bad);
    obs_data_release(invalid_settings);
    const bool fail_closed=failure.status=="error" &&
        !failure.available && failure.windows==0 &&
        failure.label.empty() && dry_audio.frames>=hz/5 &&
        dry_audio.l2.load()>0.001 && dry_audio.r2.load()>0.001;
    std::printf("SOUND_YAMNET_REAL_OBS_BAD_MODEL fail_closed=%d status=%s dry_frames=%llu\n",
       int(fail_closed),failure.status.c_str(),
       (unsigned long long)dry_audio.frames.load());

    // Stop feeding and ensure a prior sound label becomes unavailable.
    std::this_thread::sleep_for(std::chrono::milliseconds(2800));
    const Result stale=snapshot(filter);
    const bool stale_safe=stale.status=="stale"&&!stale.available&&stale.label.empty();
    std::printf("SOUND_YAMNET_REAL_OBS_STALE safely_expired=%d windows=%lld\n",
                int(stale_safe),stale.windows);
    const auto total=tone_audio.frames.load();
    const double rms_l=std::sqrt(tone_audio.l2.load()/std::max<uint64_t>(total,1));
    const double rms_r=std::sqrt(tone_audio.r2.load()/std::max<uint64_t>(total,1));
    const bool dry_stereo=std::abs(rms_l-.30/std::sqrt(2.))<.025 &&
                          std::abs(rms_r-.15/std::sqrt(2.))<.025;
    std::printf("SOUND_YAMNET_REAL_OBS_DRY_STEREO rate=%u rms=%g,%g pass=%d\n",
                hz,rms_l,rms_r,int(dry_stereo));
    double stale_score=123;
    const bool category_stale=!score("Sine wave",stale_score);
    const bool ok=catalog&&class_scores&&category_stale&&props && fail_closed && stale_safe && dry_stereo &&
                  silence.available&&tone.available &&
        silence.label=="Silence" && silence.score>.50 &&
        tone.label=="Sine wave" && tone.score>.60 &&
        tone.windows>=3 && tone.dropped==0 && tone.callback_calls>100 &&
        silence_audio.frames>=20000 && tone_audio.frames>=20000 &&
        tone_audio.l2.load()>0.001 && tone_audio.r2.load()>0.001 &&
        tone.inference_ms>=0;
    std::printf("SOUND_YAMNET_REAL_OBS_AUDIO_PASS=%d\n",int(ok));
    obs_set_output_source(1,nullptr);
    obs_source_dec_active(source);
    obs_source_filter_remove(source,filter);
    obs_source_release(filter);
    obs_source_release(source);
    obs_data_release(settings);
    obs_shutdown();
#ifdef SOUND_RT_PROBE
    const bool probe_ok=rt_probe::report(350,{3});
    return ok&&probe_ok?0:1;
#else
    return ok?0:1;
#endif
}
