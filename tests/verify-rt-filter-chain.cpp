#include "rt-callback-probe.hpp"
#include "audio-dispatch-timing.hpp"
#include <util/platform.h>
#include <cassert>
#include <cmath>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
using namespace std::chrono_literals;

static std::mutex ui_mutex;
static std::vector<std::pair<obs_task_t,void*>> tasks;
static void queue_ui(obs_task_t f,void* data,bool wait){
    assert(!wait);std::lock_guard lock(ui_mutex);tasks.emplace_back(f,data);
}
static void drain_ui(){
    std::vector<std::pair<obs_task_t,void*>> copy;
    {std::lock_guard lock(ui_mutex);copy.swap(tasks);}
    for(auto& t:copy)t.first(t.second);
}
static const char* name(void*){return "RT chain fixture";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info fixture={.id="sound_rt_chain_fixture",.type=OBS_SOURCE_TYPE_INPUT,
    .output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=create,.destroy=destroy};
struct Status{std::string status,text;int64_t hops=0,sequence=0,dropped=0;};
static Status read(obs_source_t* s,const char* proc){
    calldata_t d{};Status out;
    if(proc_handler_call(obs_source_get_proc_handler(s),proc,&d)){
        if(auto* v=calldata_string(&d,"status"))out.status=v;
        if(auto* v=calldata_string(&d,"text"))out.text=v;
        out.hops=calldata_int(&d,"hops");out.sequence=calldata_int(&d,"sequence");
        out.dropped=calldata_int(&d,"dropped_samples");
    }calldata_free(&d);return out;
}
struct Capture{size_t frames=0;double energy=0;bool finite=true;};
static void capture(void* ptr,obs_source_t*,const audio_data* p,bool){
    auto& c=*static_cast<Capture*>(ptr);
    if(!p->data[0]||!p->data[1]){c.finite=false;return;}
    for(size_t i=0;i<p->frames;++i){
        float l=reinterpret_cast<const float*>(p->data[0])[i],r=reinterpret_cast<const float*>(p->data[1])[i];
        if(!std::isfinite(l)||!std::isfinite(r)||std::abs(l)>1||std::abs(r)>1)c.finite=false;
        c.energy+=double(l)*l;
    }
    c.frames+=p->frames;
}
int main(int argc,char** argv){
    // Module/data, Whisper, speech raw f32, LiteRT, DDSP models,
    // RVC helper/library/model. All local assets.
    if(argc!=10)return 2;
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    // Positive control: ensure malloc/free and blocking lock interception
    // really work on this build, including optimization and symbol binding.
    rt_probe::Stats sentinel;
    pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    rt_probe::active=&sentinel;
    void* (*volatile allocate)(size_t)=malloc;
    void (*volatile release)(void*)=free;
    auto* p=allocate(32);release(p);
    pthread_mutex_lock(&lock);pthread_mutex_unlock(&lock);
    rt_probe::active=nullptr;pthread_mutex_destroy(&lock);
    assert(sentinel.allocs==1&&sentinel.frees==1&&sentinel.locks==1);
    puts("RT_PROBE_POSITIVE_CONTROL=1");
    assert(obs_startup("en-US",nullptr,nullptr));
    obs_set_ui_task_handler(queue_ui);
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    assert(obs_reset_audio(&ai));
    obs_module_t* m=nullptr;
    assert(obs_open_module(&m,argv[1],argv[2])==MODULE_SUCCESS&&obs_init_module(m));
    for(const auto& s:rt_probe::stats)assert(s.original);
    obs_register_source(&fixture);
    auto* source=obs_source_create(fixture.id,"Chain input",nullptr,nullptr);assert(source);
    auto add=[&](const char* id,const char* title,obs_data_t* d){
        auto* f=obs_source_create(id,title,d,nullptr);obs_data_release(d);assert(f);
        obs_source_filter_add(source,f);return f;
    };
    // OBS processes filters in add order. STT sees original speech.
    auto* d=obs_data_create();obs_data_set_string(d,"model_path",argv[3]);
    obs_data_set_string(d,"whisper_device","cpu");obs_data_set_int(d,"threads",4);
    obs_data_set_int(d,"silence_ms",700);obs_data_set_int(d,"max_phrase_ms",12000);
    auto* stt=add("sound_stt_filter","Speech",d);
    d=obs_data_create();obs_data_set_string(d,"runtime",argv[5]);obs_data_set_string(d,"models",argv[6]);
    obs_data_set_double(d,"mix",.6);auto* ddsp=add("sound_ddsp_filter","Timbre",d);
    d=obs_data_create();obs_data_set_string(d,"helper",argv[7]);obs_data_set_string(d,"library",argv[8]);
    obs_data_set_string(d,"model",argv[9]);obs_data_set_double(d,"mix",.8);
    auto* rvc=add("stts_rvc_filter","Conversion",d);
    obs_set_output_source(1,source);obs_source_set_audio_mixers(source,1);
    obs_source_inc_active(source);obs_source_set_audio_active(source,true);
    obs_source_set_async_unbuffered(source,true);
    auto ready=[&]{return read(stt,"sound_stt_read_v1").status=="ready" &&
        read(ddsp,"sound_ddsp_status_v1").status=="ready" &&
        read(rvc,"sound_rvc_native_v1").status=="ready";};
    for(int i=0;i<3000&&!ready();++i){drain_ui();std::this_thread::sleep_for(20ms);}
    assert(ready());puts("RT_CHAIN_MODELS_READY=1");
    std::ifstream f(argv[4],std::ios::binary|std::ios::ate);assert(f&&f.tellg()>0);
    std::vector<float> speech(size_t(f.tellg())/sizeof(float));f.seekg(0);
    f.read(reinterpret_cast<char*>(speech.data()),speech.size()*sizeof(float));
    assert(f);
    speech.resize(speech.size()+96000,0.f);
    Capture c;obs_source_add_audio_capture_callback(source,capture,&c);
    AudioDispatchTiming timing;
    const auto start=std::chrono::steady_clock::now();const auto timestamp=os_gettime_ns();
    const size_t packets=3600; // 36 seconds with repeated speech and silence.
    std::array<float,480> l{},r{};
    int64_t seen=0;bool words=false;
    for(size_t k=0;k<packets;++k){
        for(size_t i=0;i<480;++i){l[i]=speech[(k*480+i)%speech.size()];r[i]=l[i]*.5f;}
        obs_source_audio a{};a.data[0]=reinterpret_cast<uint8_t*>(l.data());
        a.data[1]=reinterpret_cast<uint8_t*>(r.data());a.frames=480;a.speakers=SPEAKERS_STEREO;
        a.format=AUDIO_FORMAT_FLOAT_PLANAR;a.samples_per_sec=48000;a.timestamp=timestamp+k*10000000ULL;
        timing.measure(10000000,[&]{obs_source_output_audio(source,&a);});
        drain_ui();
        if(k%20==0){
            auto status=read(stt,"sound_stt_read_v1");
            if(status.sequence>seen){seen=status.sequence;words|=status.text.find("country")!=std::string::npos;}
        }
        std::this_thread::sleep_until(start+std::chrono::milliseconds((k+1)*10));
    }
    auto a=read(stt,"sound_stt_read_v1"),b=read(ddsp,"sound_ddsp_status_v1"),e=read(rvc,"sound_rvc_native_v1");
    timing.report("stt-ddsp-rvc-chain");
    bool functional=a.status=="ready"&&b.status=="ready"&&e.status=="ready"&&
        a.dropped==0&&seen>=2&&words&&b.hops>1500&&e.hops>60&&c.frames==packets*480&&c.finite&&c.energy>1;
    bool instrumented=rt_probe::report(packets);
    printf("RT_CHAIN_FUNCTIONAL stt=%s sequences=%lld words=%d drops=%lld ddsp=%s hops=%lld rvc=%s hops=%lld frames=%zu energy=%.4f finite=%d pass=%d\n",
        a.status.c_str(),(long long)seen,words,(long long)a.dropped,b.status.c_str(),(long long)b.hops,
        e.status.c_str(),(long long)e.hops,c.frames,c.energy,c.finite,functional);
    obs_source_remove_audio_capture_callback(source,capture,&c);
    for(auto* filter:{rvc,ddsp,stt}){obs_source_filter_remove(source,filter);obs_source_release(filter);}
    obs_set_output_source(1,nullptr);obs_source_dec_active(source);obs_source_release(source);
    obs_wait_for_destroy_queue();drain_ui();obs_shutdown();
    printf("RT_CHAIN_PASS=%d\n",functional&&instrumented);return functional&&instrumented?0:1;
}
