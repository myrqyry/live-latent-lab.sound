// Actual OBS source/filter audio capture against the real local GPU sidecar.
// Uses a PRIVATE stereo music fixture converted to float32 in /tmp. Neither
// the fixture nor its generated stems enter the source repository.
#include <obs.h>
#include <obs-module.h>
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
    std::atomic<uint64_t> frames{0};
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
    double early=0,late_l=0,late_r=0,diff=0;
    uint64_t nonzero=0;
    for(uint32_t i=0;i<packet->frames;++i){
        const uint64_t time=initial+i;
        if(time<11*48000)early+=double(l[i])*l[i]+double(r[i])*r[i];
        if(time>=13*48000 && time<23*48000){
            late_l+=double(l[i])*l[i];
            late_r+=double(r[i])*r[i];
            diff+=double(l[i]-r[i])*(l[i]-r[i]);
            if(std::abs(l[i])>.0001f || std::abs(r[i])>.0001f)++nonzero;
        }
    }
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
    obs_data_set_string(settings,"socket_path",model_socket.c_str());
    obs_data_set_int(settings,"stem",3); // Vocal stem; no dry duplication
    auto* filter=obs_source_create("stem_live_buffered_filter","live-vocal-test",settings,nullptr);
    if(!input || !filter)return 7;
    obs_source_filter_add(input,filter);
    obs_set_output_source(1,input);
    obs_source_set_audio_mixers(input,1);
    obs_source_inc_active(input);
    obs_source_set_audio_active(input,true);
    obs_source_set_async_unbuffered(input,true);
    Measure stats;
    obs_source_add_audio_capture_callback(input,capture,&stats);
    std::array<float,480> l{},r{};
    const uint64_t initial=os_gettime_ns();
    for(size_t round=0;round<total_frames/480;++round){
        for(size_t i=0;i<480;++i){
            const auto at=(round*480+i)*2;
            l[i]=pcm[at];r[i]=pcm[at+1];
        }
        obs_source_audio audio{};
        audio.data[0]=reinterpret_cast<uint8_t*>(l.data());
        audio.data[1]=reinterpret_cast<uint8_t*>(r.data());
        audio.frames=480;audio.speakers=SPEAKERS_STEREO;
        audio.format=AUDIO_FORMAT_FLOAT_PLANAR;audio.samples_per_sec=48000;
        audio.timestamp=initial+round*10000000ULL;
        obs_source_output_audio(input,&audio);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    obs_source_remove_audio_capture_callback(input,capture,&stats);
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
    return pass?0:1;
}
