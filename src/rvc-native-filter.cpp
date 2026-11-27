#include "framework/preferences.hpp"
// Opt-in native RVC. The GPU runtime lives in a private child process.
#include <obs-module.h>
#include <media-io/audio-io.h>
#include "framework/rvc_native_protocol.hpp"
#include "framework/rvc_window.hpp"
#include "framework/rt_wake.hpp"
#include "framework/worker_recovery.hpp"
#include "framework/sola_stitcher.hpp"
#include <thread>
#include <chrono>
#include <memory>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
extern char** environ;
namespace {
using namespace std::chrono_literals;
std::mutex resident_slot;
enum Status{Loading,Ready,Error,Idle};
struct Config{
    std::string helper,library,model,voice,index,packaged,speaker;
    bool legacy=false;
    bool operator==(const Config& o)const{
        return helper==o.helper&&library==o.library&&model==o.model&&voice==o.voice&&
            index==o.index&&packaged==o.packaged&&speaker==o.speaker&&legacy==o.legacy;
    }
};
struct State{
    size_t rate;
    rvc_native::Pipeline pipeline;
    stts::RtWake wake;
    std::atomic<bool> stop{false};
    std::atomic<int> status{Loading},pitch{0};
    std::atomic<bool> pitch_median{true};
    std::atomic<float> mix{1},ms{0},retrieval{0},rms_mix{.25f},protection{.33f};
    std::atomic<uint64_t> hops{0},retries{0},revision{0},activity{0};
    std::atomic<bool> idle_wait{false};
    std::mutex config_mutex;
    Config config;
    std::thread worker;
    bool started=false;
    uint64_t expected=0;
    explicit State(size_t r):rate(r),pipeline(r){}
};
struct Child{
    State& s;const Config& config;const uint64_t revision;
    int fd=-1;pid_t pid=0;uint32_t seq=0;
    explicit Child(State& state,const Config& c,uint64_t rev):s(state),config(c),revision(rev){}
    ~Child(){if(fd>=0)close(fd);if(pid>0){int status;pid_t n;do{n=waitpid(pid,&status,WNOHANG);}while(n<0&&errno==EINTR);if(n==0){kill(pid,SIGKILL);do{n=waitpid(pid,&status,0);}while(n<0&&errno==EINTR);}}}
    void start(){
        int fds[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds))throw std::runtime_error("RVC socketpair");
        fd=fds[0];int child=fds[1];
        struct Close{int f;~Close(){if(f>=0)close(f);}} guard{child};
        if(fd<3||child<3)throw std::runtime_error("RVC invalid inherited descriptor");
        posix_spawn_file_actions_t actions;
        if(posix_spawn_file_actions_init(&actions))throw std::runtime_error("RVC spawn init");
        struct Actions{posix_spawn_file_actions_t* a;~Actions(){posix_spawn_file_actions_destroy(a);}} cleanup{&actions};
        if(posix_spawn_file_actions_adddup2(&actions,child,0)||posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0)||posix_spawn_file_actions_adddup2(&actions,1,2))throw std::runtime_error("RVC spawn setup");
        std::array<std::string,8> args{config.helper,config.library,config.model,"cuda",config.voice,config.index,config.packaged,config.speaker};
        std::array<char*,9> argv{};for(size_t i=0;i<args.size();++i)argv[i]=args[i].data();
        if(posix_spawn(&pid,args[0].c_str(),&actions,nullptr,argv.data(),environ)){pid=0;throw std::runtime_error("Cannot launch RVC helper");}
    }
    bool transfer(void* bytes,size_t size,bool write,std::chrono::steady_clock::time_point deadline,bool cold){
        auto* p=static_cast<char*>(bytes);
        while(size&&!s.stop.load()&&s.revision.load()==revision&&(cold||!s.pipeline.failed())&&std::chrono::steady_clock::now()<deadline){
            pollfd e{fd,short(write?POLLOUT:POLLIN),0};int n=poll(&e,1,20);
            if(n<0&&errno==EINTR)continue;if(n<0)return false;if(!n)continue;
            if(!(e.revents&(write?POLLOUT:POLLIN)))return false;
            auto m=write?send(fd,p,size,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,p,size,MSG_DONTWAIT);
            if(m<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
            if(m<=0)return false;p+=m;size-=size_t(m);
        }
        return size==0;
    }
    void convert(std::array<float,16000>& pcm,int pitch,bool cold,float blend,std::vector<float>& out){
        using namespace rvc_native;
        Request r{magic,++seq,uint32_t(pcm.size()),uint32_t(s.rate),pitch,blend,s.rms_mix.load(),s.protection.load(),s.pitch_median.load()?1u:0u};Reply q{};
        auto end=std::chrono::steady_clock::now()+(cold?45s:1500ms);
        if(!transfer(&r,sizeof r,true,end,cold)||!transfer(pcm.data(),sizeof(pcm),true,end,cold)||!transfer(&q,sizeof q,false,end,cold)||q.magic!=magic||q.sequence!=r.sequence||q.status||q.frames<s.rate*88/100||q.frames>s.rate)throw std::runtime_error("RVC worker unavailable or rejected audio");
        out.resize(q.frames);
        if(!transfer(out.data(),out.size()*sizeof(float),false,end,cold)||!std::all_of(out.begin(),out.end(),[](float v){return std::isfinite(v)&&std::abs(v)<=1;}))throw std::runtime_error("RVC invalid audio");
    }
};
void run(State* s){
    stts::WorkerRecovery budget(60); // 30 seconds of actual 500 ms hops
    uint64_t attempt_revision=0;
    while(!s->stop.load()){
        Config config;uint64_t revision;
        {
            std::lock_guard<std::mutex> lock(s->config_mutex);
            config=s->config;revision=s->revision.load();
            s->status.store(Loading,std::memory_order_release);
        }
        if(attempt_revision!=revision){attempt_revision=revision;budget.reset();}
        bool initialized=false,idle=false;
        uint64_t idle_activity=0;
        try{
            if(config.legacy)throw std::runtime_error("Previous ONNX voice selection; explicitly select a native model");
            std::unique_lock<std::mutex> slot(resident_slot,std::try_to_lock);
            if(!slot.owns_lock())throw std::runtime_error("Another RVC filter owns the CUDA session");
            Child worker(*s,config,revision);worker.start();
            std::array<float,16000> warm{};
            for(size_t i=0;i<warm.size();++i)warm[i]=.05f*std::sin(6.28318530718*220*i/16000.);
            std::vector<float> output;output.reserve(s->rate);
            const auto blend=[&]{return config.index.empty()&&!config.voice.empty()?0.f:s->retrieval.load();};
            worker.convert(warm,0,true,config.index.empty()?blend():0.5f,output);
            rvc_native::Window window(s->rate);
            std::vector<float> l(s->rate),r(s->rate);
            stts::SolaStitcher stitch({s->rate/2,s->rate/25,s->rate/50});
            std::vector<float> hop(s->rate/2);
            {
                std::lock_guard<std::mutex> lock(s->config_mutex);
                if(s->stop.load())return;
                if(s->revision.load()!=revision)continue;
                if(s->pipeline.failed()&&!s->pipeline.restart())
                    throw std::runtime_error("RVC recovery epoch unavailable");
                initialized=true;
                s->status.store(Ready,std::memory_order_release);
            }
            auto last=std::chrono::steady_clock::now();
            auto activity=s->activity.load();
            while(!s->stop.load()&&!s->pipeline.failed()&&s->revision.load()==revision){
                const auto ticket=s->wake.sequence();
                const auto current=s->activity.load();
                if(current!=activity){activity=current;last=std::chrono::steady_clock::now();}
                auto n=window.needed();
                if(!s->pipeline.worker_prepare()||!s->pipeline.read(l.data(),r.data(),n)){
                    if(std::chrono::steady_clock::now()-last>30s){
                        idle=true;idle_activity=activity;
                        s->pipeline.fail();break;
                    }
                    if(!s->stop.load()&&!s->pipeline.failed())s->wake.wait(ticket,100);
                    continue;
                }
                auto t=std::chrono::steady_clock::now();auto pcm=window.push(l.data(),r.data(),n);
                worker.convert(pcm,s->pitch.load(),false,blend(),output);
                auto offset=s->rate*24/100;
                stitch.push_into(output.data()+offset,output.size()-offset,hop.data(),hop.size());
                if(!s->pipeline.write(hop.data()))break;
                s->ms=float(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count());
                ++s->hops;budget.on_success();
            }
        }catch(const std::exception& e){
            if(!s->stop.load()&&s->revision.load()==revision)
                blog(LOG_WARNING,"[Sound RVC] %s",e.what());
        }catch(...){
            if(!s->stop.load()&&s->revision.load()==revision)
                blog(LOG_WARNING,"[Sound RVC] Unknown worker failure");
        }
        // The previous child has been reaped and the GPU slot released.
        if(s->stop.load())return;
        bool retry=false;
        {
            std::lock_guard<std::mutex> lock(s->config_mutex);
            if(s->revision.load()!=revision)continue;
            s->pipeline.fail();s->ms=0;
            s->status.store(idle?Idle:Error,std::memory_order_release);
            budget.on_fault();
            retry=!idle&&budget.can_retry(initialized);
        }
        // Idle is not a fault and does not spend retries or hold CUDA.
        // Wait for actual source activity, a settings revision or shutdown.
        if(idle){
            s->idle_wait.store(true,std::memory_order_release);
            while(!s->stop.load()&&s->revision.load()==revision){
                const auto ticket=s->wake.sequence();
                if(s->activity.load()!=idle_activity)break;
                if(!s->stop.load())s->wake.wait(ticket,100);
            }
            s->idle_wait.store(false,std::memory_order_release);
            continue;
        }
        const auto until=std::chrono::steady_clock::now()+
            std::chrono::milliseconds(retry?budget.delay_ms():0);
        while(!s->stop.load()){
            const auto ticket=s->wake.sequence();
            {
                std::lock_guard<std::mutex> lock(s->config_mutex);
                if(s->revision.load()!=revision)break;
                if(retry&&std::chrono::steady_clock::now()>=until){
                    budget.on_retry();++s->retries;break;
                }
            }
            if(!s->stop.load())s->wake.wait(ticket,100);
        }
    }
}
const char* name(void*){return "LLL.sound Voice Conversion (RVC CUDA)";}
void defaults(obs_data_t* d){
    obs_data_set_default_double(d,"mix",1);obs_data_set_default_int(d,"pitch",0);obs_data_set_default_double(d,"retrieval",0);
    obs_data_set_default_string(d,"packaged_voice","default");obs_data_set_default_int(d,"speaker_id",0);
    obs_data_set_default_double(d,"rms_mix",.25);obs_data_set_default_double(d,"unvoiced_protection",.33);obs_data_set_default_bool(d,"pitch_median",true);
    if(const char* home=std::getenv("HOME")){
        std::string base=std::string(home)+"/.local/share/live-latent-lab-sound/rvc";
        obs_data_set_default_string(d,"helper",(base+"/rvc-native-worker-v3").c_str());
        obs_data_set_default_string(d,"library",(base+"/libaudiocpp.so.0.1.0").c_str());
        obs_data_set_default_string(d,"model",(base+"/rvc-f16.gguf").c_str());
    }
}
void update(void* ptr,obs_data_t* d){
    auto* s=static_cast<State*>(ptr);s->pitch=int(std::clamp<int64_t>(obs_data_get_int(d,"pitch"),-24,24));
    double retrieval=obs_data_get_double(d,"retrieval");s->retrieval=std::isfinite(retrieval)?std::clamp(float(retrieval),0.f,1.f):0.f;
    auto unit=[&](const char* k,float fallback){double v=obs_data_get_double(d,k);return std::isfinite(v)?std::clamp(float(v),0.f,1.f):fallback;};
    s->rms_mix=unit("rms_mix",.25f);s->protection=unit("unvoiced_protection",.33f);s->pitch_median=obs_data_get_bool(d,"pitch_median");
    double mix=obs_data_get_double(d,"mix");s->mix=std::isfinite(mix)?std::clamp(float(mix),0.f,1.f):0;
    Config c{obs_data_get_string(d,"helper"),obs_data_get_string(d,"library"),
        obs_data_get_string(d,"model"),obs_data_get_string(d,"voice_checkpoint"),
        obs_data_get_string(d,"retrieval_index"),obs_data_get_string(d,"packaged_voice"),
        std::to_string(std::clamp<int64_t>(obs_data_get_int(d,"speaker_id"),0,65535))};
    c.legacy=obs_data_has_user_value(d,"voice_path")&&*obs_data_get_string(d,"voice_path")&&
        !obs_data_has_user_value(d,"model")&&c.voice.empty();
    std::lock_guard<std::mutex> lock(s->config_mutex);
    if(!(c==s->config)||!s->revision.load()){
        if(s->revision.load())s->pipeline.fail();
        s->config=std::move(c);++s->revision;
        s->status.store(Loading,std::memory_order_release);s->wake.notify();
    }
}
bool retry_models(obs_properties_t*,obs_property_t*,void* data){
    auto* s=static_cast<State*>(data);if(!s)return false;
    std::lock_guard<std::mutex> lock(s->config_mutex);
    s->pipeline.fail();++s->revision;s->status.store(Loading,std::memory_order_release);
    s->wake.notify();return true;
}
obs_properties_t* properties(void* ptr){
    auto* p=obs_properties_create();
    obs_properties_add_path(p,"helper","Native worker",OBS_PATH_FILE,"All files (*.*)",sound_preferences::directory(true).c_str());
    obs_properties_add_path(p,"library","CUDA runtime",OBS_PATH_FILE,"Shared library (*.so*)",sound_preferences::directory(true).c_str());
    obs_properties_add_path(p,"model","RVC GGUF package",OBS_PATH_FILE,"GGUF (*.gguf)",sound_preferences::directory(false).c_str());
    std::string root;
    if(const char* home=std::getenv("HOME"))root=std::string(home)+"/.local/share/live-latent-lab-sound/rvc/voice-library";
    auto* voices=obs_properties_add_list(p,"packaged_voice","Packaged voice (if no checkpoint)",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    for(auto id:{"default","manthos","chocola","fraise"})obs_property_list_add_string(voices,id,id);
    obs_properties_add_int(p,"speaker_id","Speaker ID (checkpoint dependent)",0,65535,1);
    obs_properties_add_path(p,"voice_checkpoint","Voice checkpoint (optional)",OBS_PATH_FILE,"RVC checkpoints (*.pth *.pt)",root.c_str());
    obs_properties_add_path(p,"retrieval_index","Retrieval index (optional)",OBS_PATH_FILE,"FAISS index (*.index)",root.c_str());
    obs_properties_add_float_slider(p,"retrieval","Retrieval blend",0,1,.01);
    obs_properties_add_float_slider(p,"rms_mix","RMS envelope mix (0=input, 1=converted)",0,1,.01);
    obs_properties_add_float_slider(p,"unvoiced_protection","Unvoiced protection (lower=stronger; ≥0.5 off)",0,1,.01);
    obs_properties_add_bool(p,"pitch_median","RMVPE median filtering");
    obs_properties_add_text(p,"note","Monophonic CUDA conversion. Empty checkpoint uses the selected packaged voice and its embedded index. Custom voices need a matching index for retrieval. Voice, speaker ID, index and runtime paths reload live. RMVPE is the only pitch extractor. 1.5 s fixed delay after loading. Faults retain aligned delayed dry and retry up to three times. Idle worker releases CUDA and resumes on input. Retry models to reload explicitly. Only use voices you are entitled to use.",OBS_TEXT_INFO);
    obs_properties_add_button2(p,"retry_models","Retry models",retry_models,ptr);
    if(auto* s=static_cast<State*>(ptr)){
        auto status=s->status.load();
        obs_properties_add_text(p,"status",status==Loading?"Loading: dry audio":status==Idle?"Idle: resumes on input":status==Ready&&!s->pipeline.failed()?"Ready: native CUDA":"Error: dry audio (retry models)",OBS_TEXT_INFO);
    }
    obs_properties_add_int_slider(p,"pitch","Pitch shift (semitones)",-24,24,1);
    obs_properties_add_float_slider(p,"mix","Wet / dry",0,1,.01);return p;
}
void snapshot(void* ptr,calldata_t* d){
    auto* s=static_cast<State*>(ptr);auto status=s->status.load();
    calldata_set_string(d,"status",status==Idle?"idle":status==Loading?"loading":status==Error||s->pipeline.failed()?"error":"ready");
    calldata_set_int(d,"hops",s->hops.load());calldata_set_float(d,"processing_ms",s->ms.load());calldata_set_int(d,"delay_ms",1500);
}
void recovery_snapshot(void* ptr,calldata_t* d){
    auto* s=static_cast<State*>(ptr);
    calldata_set_int(d,"automatic_retries",s->retries.load());
}
void* create(obs_data_t* d,obs_source_t* source){
    auto* audio=obs_get_audio();if(!audio)return nullptr;auto rate=audio_output_get_sample_rate(audio);auto ch=audio_output_get_channels(audio);
    if((rate!=44100&&rate!=48000)||(ch!=1&&ch!=2))return nullptr;
    try{
        auto s=std::make_unique<State>(rate);update(s.get(),d);
        s->worker=std::thread(run,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),"void sound_rvc_native_v1(out string status, out int hops, out float processing_ms, out int delay_ms)",snapshot,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),"void sound_rvc_recovery_v1(out int automatic_retries)",recovery_snapshot,s.get());
        return s.release();
    }catch(...){return nullptr;}
}
void destroy(void* ptr){auto* s=static_cast<State*>(ptr);s->stop=true;s->wake.notify();if(s->worker.joinable())s->worker.join();delete s;}
obs_audio_data* audio(void* ptr,obs_audio_data* a){
    auto* s=static_cast<State*>(ptr);if(!a||!a->data[0]||!a->frames)return a;
    ++s->activity;
    if(s->idle_wait.load(std::memory_order_acquire))s->wake.notify();
    if(!s->started){if(s->status.load(std::memory_order_acquire)!=Ready)return a;s->started=true;}
    const bool was_failed=s->pipeline.failed();
    if(s->expected&&(a->timestamp>s->expected?a->timestamp-s->expected:s->expected-a->timestamp)>80000000)s->pipeline.fail();
    s->expected=a->timestamp+uint64_t(a->frames)*1000000000/s->rate;
    s->pipeline.process(reinterpret_cast<float*>(a->data[0]),reinterpret_cast<float*>(a->data[1]),a->frames,s->mix.load());
    if((!was_failed&&s->pipeline.failed())||s->pipeline.input_ready())s->wake.notify();
    return a;
}
}
void register_sound_rvc(){
    obs_source_info info{};info.id="stts_rvc_filter";info.type=OBS_SOURCE_TYPE_FILTER;info.output_flags=OBS_SOURCE_AUDIO;
    info.get_name=name;info.create=create;info.destroy=destroy;info.get_defaults=defaults;info.get_properties=properties;info.update=update;info.filter_audio=audio;obs_register_source(&info);
}
