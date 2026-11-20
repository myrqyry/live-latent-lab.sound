// Tests the real compiled OBS MeanVC2 bridge (not a mock).
// A deterministic speech fixture sends 48 kHz planar audio through libobs.
// The bridge captures it, talks to a separate native MeanVC2 process,
// and OBS captures the bridge's returned converted output.
#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <array>

struct Stats {
    std::atomic<uint64_t> frames{0};
    std::atomic<double> energy{0};
    std::atomic<float> peak{0};
    std::atomic<size_t> written{0};
    std::vector<float> recorded;
};
static void capture(void *param,obs_source_t*,const struct audio_data *data,bool muted) {
    if(muted||!data||!data->data[0])return;
    auto &s=*static_cast<Stats*>(param);
    const float *p=(const float*)data->data[0];
    double energy=0;float peak=0;
    for(uint32_t i=0;i<data->frames;++i) {
        if(!std::isfinite(p[i]))continue;
        energy+=(double)p[i]*p[i];peak=std::max(peak,std::abs(p[i]));
    }
    const size_t pos=s.written.fetch_add(data->frames);
    if(pos+data->frames<=s.recorded.size())
        std::copy(p,p+data->frames,s.recorded.begin()+static_cast<ptrdiff_t>(pos));
    s.frames.fetch_add(data->frames);
    double old=s.energy.load();
    while(!s.energy.compare_exchange_weak(old,old+energy)){}
    float prev=s.peak.load();
    while(prev<peak && !s.peak.compare_exchange_weak(prev,peak)){}
}
static const char* fixture_name(void*) {return "MeanVC2 real recorded-speech fixture";}
static void* fixture_create(obs_data_t*,obs_source_t*) {return reinterpret_cast<void*>(1);}
static void fixture_destroy(void*){}
static obs_source_info fixture=[] {
    obs_source_info s{};
    s.id="test_meanvc2_recorded_speech";
    s.type=OBS_SOURCE_TYPE_INPUT;
    s.output_flags=OBS_SOURCE_AUDIO;
    s.get_name=fixture_name;s.create=fixture_create;s.destroy=fixture_destroy;
    return s;
}();
static std::vector<float> read_pcm(const char *path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)return {};
    size_t bytes=(size_t)file.tellg();
    if(bytes==0||bytes%4)return {};
    std::vector<float> pcm(bytes/4);
    file.seekg(0);file.read((char*)pcm.data(),bytes);
    if(!file)return {};
    return pcm;
}
static void produce(obs_source_t *source,const std::vector<float>&samples) {
    static constexpr uint32_t N=480;
    std::array<float,N> left{},right{};
    const uint64_t started=os_gettime_ns();
    size_t k=0;
    for(size_t pos=0;pos+N<=samples.size();pos+=N,++k) {
        for(uint32_t i=0;i<N;++i) {
            left[i]=samples[pos+i];
            right[i]=samples[pos+i]*0.9f;
        }
        obs_source_audio packet{};
        packet.data[0]=(uint8_t*)left.data();
        packet.data[1]=(uint8_t*)right.data();
        packet.frames=N;
        packet.speakers=SPEAKERS_STEREO;
        packet.format=AUDIO_FORMAT_FLOAT_PLANAR;
        packet.samples_per_sec=48000;
        packet.timestamp=started+k*10000000ULL;
        obs_source_output_audio(source,&packet);
        std::this_thread::sleep_until(std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(10));
    }
}
int main(int argc,char **argv) {
    if(argc!=5 && argc!=6)return 2;
    const bool expect_silence=(argc==6 && std::string(argv[5])=="--expect-no-sidecar");
    const std::string flag=argc==6?argv[5]:"";
    const std::string prefix="--capture-f32=";
    const std::string capture_path=flag.rfind(prefix,0)==0?flag.substr(prefix.size()):"";
    if(argc==6 && !expect_silence && capture_path.empty())return 2;
    auto speech=read_pcm(argv[3]);
    if(speech.size()<48000)return 3;
    if(!obs_startup("en-US",nullptr,nullptr))return 4;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 5;
    obs_module_t *module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS || !obs_init_module(module))return 6;
    obs_register_source(&fixture);
    auto *fixture_src=obs_source_create("test_meanvc2_recorded_speech","MVC2-input",nullptr,nullptr);
    auto *settings=obs_data_create();
    obs_data_set_string(settings,"input_name","MVC2-input");
    obs_data_set_string(settings,"socket_path",argv[4]);
    auto *bridge=obs_source_create("meanvc2_bridge_source","MVC2-output",settings,nullptr);
    if(!fixture_src||!bridge)return 7;
    obs_properties_t *props=obs_get_source_properties("meanvc2_bridge_source");
    bool props_ok=props && obs_properties_get(props,"input_name") &&
                            obs_properties_get(props,"socket_path");
    if(props)obs_properties_destroy(props);
    obs_set_output_source(0,fixture_src);
    obs_set_output_source(1,bridge);
    obs_source_set_audio_mixers(fixture_src,1);
    obs_source_set_audio_mixers(bridge,1);
    obs_source_inc_active(fixture_src);
    obs_source_inc_active(bridge);
    obs_source_set_audio_active(fixture_src,true);
    obs_source_set_audio_active(bridge,true);
    obs_source_set_async_unbuffered(fixture_src,true);
    obs_source_set_async_unbuffered(bridge,true);
    Stats output;
    // Source length is known for the test; leave space for legitimate
    // synthesizer tail rather than silently truncating long auditions.
    output.recorded.resize(speech.size()+48000u*5u);
    obs_source_add_audio_capture_callback(bridge,capture,&output);
    produce(fixture_src,speech);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    obs_source_remove_audio_capture_callback(bridge,capture,&output);
    const uint64_t frames=output.frames.load();
    const double rms=std::sqrt(output.energy.load()/std::max<uint64_t>(1,frames));
    const double duration=frames/48000.0;
    const bool ok=props_ok && (expect_silence
             ? (frames==0 && output.peak.load()==0.0f)
             : (frames>48000 && rms>0.0003 && rms<1.0 &&
                output.peak.load()>0.002 && output.peak.load()<10.0));
    if(!capture_path.empty() && ok) {
        if(output.written.load()>output.recorded.size()) {
            std::fprintf(stderr,"MVC2_OBS_CAPTURE_OVERFLOW=1\n");
            return 11;
        }
        std::ofstream file(capture_path,std::ios::binary);
        const size_t recorded=std::min(output.written.load(),output.recorded.size());
        file.write((char*)output.recorded.data(),recorded*sizeof(float));
        if(!file)return 10;
        std::printf("MVC2_OBS_CAPTURED_F32=%zu\n",recorded);
    }
    std::printf("MVC2_OBS_CAPTURE expecting_silence=%d properties=%d input_frames=%zu converted_frames=%llu "
                "converted_seconds=%g rms=%g peak=%g pass=%d\n",
                expect_silence,props_ok,speech.size(),(unsigned long long)frames,
                duration,rms,output.peak.load(),ok);
    obs_set_output_source(1,nullptr);
    obs_set_output_source(0,nullptr);
    obs_source_dec_active(bridge);
    obs_source_dec_active(fixture_src);
    obs_source_release(bridge);
    obs_source_release(fixture_src);
    obs_data_release(settings);
    obs_shutdown();
    return ok?0:1;
}
