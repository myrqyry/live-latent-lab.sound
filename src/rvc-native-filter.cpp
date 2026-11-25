// Opt-in native RVC. The GPU runtime lives in a private child process.
#include <obs-module.h>
#include <media-io/audio-io.h>
#include "framework/rvc_native_protocol.hpp"
#include "framework/rvc_window.hpp"
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
enum Status{Loading,Ready,Error};
struct State{
    size_t rate;
    rvc_native::Pipeline pipeline;
    std::atomic<bool> stop{false};
    std::atomic<int> status{Loading},pitch{0};
    std::atomic<float> mix{1},ms{0};
    std::atomic<uint64_t> hops{0};
    std::string helper,library,model;
    std::thread worker;
    bool started=false;
    uint64_t expected=0;
    explicit State(size_t r):rate(r),pipeline(r){}
};
struct Child{
    State& s;int fd=-1;pid_t pid=0;uint32_t seq=0;
    explicit Child(State& state):s(state){}
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
        std::array<std::string,4> args{s.helper,s.library,s.model,"cuda"};
        std::array<char*,5> argv{};for(size_t i=0;i<args.size();++i)argv[i]=args[i].data();
        if(posix_spawn(&pid,args[0].c_str(),&actions,nullptr,argv.data(),environ)){pid=0;throw std::runtime_error("Cannot launch RVC helper");}
    }
    bool transfer(void* bytes,size_t size,bool write,std::chrono::steady_clock::time_point deadline){
        auto* p=static_cast<char*>(bytes);
        while(size&&!s.stop.load()&&!s.pipeline.failed()&&std::chrono::steady_clock::now()<deadline){
            pollfd e{fd,short(write?POLLOUT:POLLIN),0};int n=poll(&e,1,20);
            if(n<0&&errno==EINTR)continue;if(n<0)return false;if(!n)continue;
            if(!(e.revents&(write?POLLOUT:POLLIN)))return false;
            auto m=write?send(fd,p,size,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,p,size,MSG_DONTWAIT);
            if(m<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
            if(m<=0)return false;p+=m;size-=size_t(m);
        }
        return size==0;
    }
    std::vector<float> convert(std::array<float,16000>& pcm,int pitch,bool cold){
        using namespace rvc_native;
        Request r{magic,++seq,uint32_t(pcm.size()),uint32_t(s.rate),pitch};Reply q{};
        auto end=std::chrono::steady_clock::now()+(cold?45s:1500ms);
        if(!transfer(&r,sizeof r,true,end)||!transfer(pcm.data(),sizeof(pcm),true,end)||!transfer(&q,sizeof q,false,end)||q.magic!=magic||q.sequence!=r.sequence||q.status||q.frames<s.rate*88/100||q.frames>s.rate)throw std::runtime_error("RVC worker unavailable or rejected audio");
        std::vector<float> out(q.frames);
        if(!transfer(out.data(),out.size()*sizeof(float),false,end)||!std::all_of(out.begin(),out.end(),[](float v){return std::isfinite(v)&&std::abs(v)<=1;}))throw std::runtime_error("RVC invalid audio");
        return out;
    }
};
void run(State* s){
    try{
        std::unique_lock<std::mutex> slot(resident_slot,std::try_to_lock);
        if(!slot.owns_lock())throw std::runtime_error("Another RVC filter owns the CUDA session");
        Child worker(*s);worker.start();
        std::array<float,16000> warm{};for(size_t i=0;i<warm.size();++i)warm[i]=.05f*std::sin(6.28318530718*220*i/16000.);
        worker.convert(warm,0,true);
        rvc_native::Window window(s->rate);
        std::vector<float> l(s->rate),r(s->rate);
        stts::SolaStitcher stitch({s->rate/2,s->rate/25,s->rate/50});
        s->status.store(Ready,std::memory_order_release);
        auto last=std::chrono::steady_clock::now();
        while(!s->stop.load()&&!s->pipeline.failed()){
            auto n=window.needed();
            if(!s->pipeline.read(l.data(),r.data(),n)){
                if(std::chrono::steady_clock::now()-last>30s)throw std::runtime_error("RVC input idle; recreate filter to restart");
                std::this_thread::sleep_for(2ms);continue;
            }
            auto t=std::chrono::steady_clock::now();auto pcm=window.push(l.data(),r.data(),n);
            // Always convert the real window, including silence; never reuse stale output.
            auto output=worker.convert(pcm,s->pitch.load(),false);
            auto offset=s->rate*24/100;
            auto hop=stitch.push(output.data()+offset,output.size()-offset);
            if(!s->pipeline.write(hop.pcm.data()))break;
            s->ms=float(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count());
            ++s->hops;last=std::chrono::steady_clock::now();
        }
        if(s->pipeline.failed())s->status=Error;
    }catch(const std::exception& e){blog(LOG_WARNING,"[Sound RVC] %s",e.what());s->status=Error;s->pipeline.fail();}
}
const char* name(void*){return "LLL.sound Voice Conversion (RVC CUDA)";}
void defaults(obs_data_t* d){
    obs_data_set_default_double(d,"mix",1);obs_data_set_default_int(d,"pitch",0);
    if(const char* home=std::getenv("HOME")){
        std::string base=std::string(home)+"/.local/share/live-latent-lab-sound/rvc";
        obs_data_set_default_string(d,"helper",(base+"/rvc-native-worker").c_str());
        obs_data_set_default_string(d,"library",(base+"/libaudiocpp.so.0.1.0").c_str());
        obs_data_set_default_string(d,"model",(base+"/rvc-f16.gguf").c_str());
    }
}
void update(void* ptr,obs_data_t* d){
    auto* s=static_cast<State*>(ptr);s->pitch=int(std::clamp<int64_t>(obs_data_get_int(d,"pitch"),-24,24));
    double mix=obs_data_get_double(d,"mix");s->mix=std::isfinite(mix)?std::clamp(float(mix),0.f,1.f):0;
}
obs_properties_t* properties(void* ptr){
    auto* p=obs_properties_create();
    obs_properties_add_path(p,"helper","Native worker",OBS_PATH_FILE,"All files (*.*)",nullptr);
    obs_properties_add_path(p,"library","CUDA runtime",OBS_PATH_FILE,"Shared library (*.so*)",nullptr);
    obs_properties_add_path(p,"model","RVC GGUF package",OBS_PATH_FILE,"GGUF (*.gguf)",nullptr);
    obs_properties_add_text(p,"note","Experimental monophonic conversion; default packaged voice, CUDA only. 1.5 s fixed delay after loading. Paths apply on recreation. Failure after activation preserves delayed dry. Recreate to retry. Only use voices you are entitled to use.",OBS_TEXT_INFO);
    if(auto* s=static_cast<State*>(ptr)){
        auto status=s->status.load();
        obs_properties_add_text(p,"status",s->pipeline.failed()?"Failed: dry audio; recreate filter":status==Ready?"Ready: native CUDA":status==Loading?"Loading: dry audio":"Error: dry audio",OBS_TEXT_INFO);
    }
    obs_properties_add_int_slider(p,"pitch","Pitch shift (semitones)",-24,24,1);
    obs_properties_add_float_slider(p,"mix","Wet / dry",0,1,.01);return p;
}
void snapshot(void* ptr,calldata_t* d){
    auto* s=static_cast<State*>(ptr);auto status=s->status.load();
    calldata_set_string(d,"status",s->pipeline.failed()||status==Error?"error":status==Ready?"ready":"loading");
    calldata_set_int(d,"hops",s->hops.load());calldata_set_float(d,"processing_ms",s->ms.load());calldata_set_int(d,"delay_ms",1500);
}
void* create(obs_data_t* d,obs_source_t* source){
    auto* audio=obs_get_audio();if(!audio)return nullptr;auto rate=audio_output_get_sample_rate(audio);auto ch=audio_output_get_channels(audio);
    if((rate!=44100&&rate!=48000)||(ch!=1&&ch!=2))return nullptr;
    try{
        auto s=std::make_unique<State>(rate);s->helper=obs_data_get_string(d,"helper");s->library=obs_data_get_string(d,"library");s->model=obs_data_get_string(d,"model");update(s.get(),d);
        if(obs_data_has_user_value(d,"voice_path")&&*obs_data_get_string(d,"voice_path")&&!obs_data_has_user_value(d,"model")){
            s->status=Error;s->pipeline.fail();
            blog(LOG_WARNING,"[Sound RVC] Previous ONNX voice selection is not a native GGUF package; explicitly select a native model and recreate the filter.");
        }else s->worker=std::thread(run,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),"void sound_rvc_native_v1(out string status, out int hops, out float processing_ms, out int delay_ms)",snapshot,s.get());
        return s.release();
    }catch(...){return nullptr;}
}
void destroy(void* ptr){auto* s=static_cast<State*>(ptr);s->stop=true;if(s->worker.joinable())s->worker.join();delete s;}
obs_audio_data* audio(void* ptr,obs_audio_data* a){
    auto* s=static_cast<State*>(ptr);if(!a||!a->data[0]||!a->frames)return a;
    if(!s->started){if(s->status.load(std::memory_order_acquire)!=Ready)return a;s->started=true;}
    if(s->expected&&(a->timestamp>s->expected?a->timestamp-s->expected:s->expected-a->timestamp)>80000000)s->pipeline.fail();
    s->expected=a->timestamp+uint64_t(a->frames)*1000000000/s->rate;
    s->pipeline.process(reinterpret_cast<float*>(a->data[0]),reinterpret_cast<float*>(a->data[1]),a->frames,s->mix.load());return a;
}
}
void register_sound_rvc(){
    obs_source_info info{};info.id="stts_rvc_filter";info.type=OBS_SOURCE_TYPE_FILTER;info.output_flags=OBS_SOURCE_AUDIO;
    info.get_name=name;info.create=create;info.destroy=destroy;info.get_defaults=defaults;info.get_properties=properties;info.update=update;info.filter_audio=audio;obs_register_source(&info);
}
