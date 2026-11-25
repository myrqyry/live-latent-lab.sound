#include <obs-module.h>
#include <util/platform.h>
#include <libsa3_v1.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
constexpr uint32_t rate=44100;
constexpr int max_seconds=60;
// Limit simultaneous diffusion jobs, including separate OBS sources.
std::timed_mutex inference_gate;
struct Config {
    std::string models,prompt,negative,device;
    int steps=8;
    int64_t seed=-1;
    double seconds=12;
};
struct Clip {
    std::vector<float> pcm;
    size_t frames=0;
};
enum class Job { None, Generate, Replay };
struct State {
    explicit State(obs_source_t* s):source(s){}
    obs_source_t* source;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    Config config;
    Job pending=Job::None;
    std::shared_ptr<const Clip> saved;
    std::atomic<bool> active{false},enabled{true},closing{false};
    std::atomic<uint64_t> epoch{0},started{0},completed{0},played{0},failed{0};
    std::atomic<int> progress{0};
    std::string message="Ready";
    bool current(uint64_t e) const {return !closing&&active&&enabled&&epoch==e;}
};
void message(State& s,uint64_t epoch,const std::string& value){
    std::lock_guard lock(s.mutex);if(s.current(epoch))s.message=value;
}
struct Cancel {State& state;uint64_t epoch;};
int SA3_CALL cancelled(void* p){
    auto& c=*static_cast<Cancel*>(p);return !c.state.current(c.epoch);
}
void SA3_CALL progress(void* p,const sa3_progress_v1* v){
    auto& c=*static_cast<Cancel*>(p);
    if(v&&c.state.current(c.epoch)&&std::isfinite(v->fraction))
        c.state.progress=int(std::clamp(v->fraction,0.f,1.f)*100);
}
struct Runtime {
    void* lib=nullptr;
    const sa3_api_v1* api=nullptr;
    sa3_context* ctx=nullptr;
    ~Runtime(){if(ctx)api->context_destroy(ctx);if(lib)dlclose(lib);}
    Runtime()=default;
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;
    bool open(const Config& c,std::string& error){
        auto path=std::filesystem::path(obs_get_module_binary_path(obs_current_module())).parent_path()/"sa3-runtime/libsa3.so";
        // Private SONAMEs plus local deep binding prevent an already-loaded
        // GGML consumer from satisfying this runtime's internal symbols.
        lib=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_DEEPBIND|RTLD_NODELETE);
        if(!lib){error=dlerror();return false;}
        auto get=reinterpret_cast<const sa3_api_v1*(*)(uint32_t)>(dlsym(lib,"sa3_get_api"));
        api=get?get(SA3_ABI_VERSION_1):nullptr;
        if(!api||api->abi_version!=SA3_ABI_VERSION_1||
           api->size<offsetof(sa3_api_v1,convert_lora)+sizeof(api->convert_lora)||
           !api->context_config_init||!api->error_init||!api->context_create||
           !api->context_destroy||!api->request_init||!api->result_init||
           !api->result_free||!api->generate){error="Incompatible SA3 ABI";return false;}
        sa3_context_config_v1 cfg{};cfg.size=sizeof cfg;api->context_config_init(&cfg);
        cfg.models_dir=c.models.c_str();cfg.variant="small-music";cfg.dit_encoding="q4_k_m";
        cfg.text_encoder_encoding="f16";cfg.autoencoder_encoding="f16";
        cfg.device=c.device=="cpu"?"cpu":nullptr;cfg.cpu_threads=8;
        sa3_error_v1 e{};e.size=sizeof e;api->error_init(&e);
        if(api->context_create(&cfg,&ctx,&e)!=SA3_STATUS_OK_V1){error=e.message;return false;}
        return true;
    }
};
std::shared_ptr<Clip> generate(State& s,const Config& c,uint64_t epoch,std::string& error){
    if(c.models.empty()||c.prompt.empty()){error="Choose a model directory and enter a prompt";return {};}
    if(c.prompt.size()>8192||c.negative.size()>8192){error="Prompt exceeds 8192 bytes";return {};}
    std::unique_lock<std::timed_mutex> gate(inference_gate,std::defer_lock);
    while(s.current(epoch)&&!gate.try_lock_for(50ms)){}
    if(!s.current(epoch)||!gate.owns_lock())return {};
    Runtime runtime;
    if(!runtime.open(c,error)||!s.current(epoch))return {};
    const auto* api=runtime.api;
    Cancel token{s,epoch};
    sa3_request_v1 req{};req.size=sizeof req;api->request_init(&req);
    req.prompt=c.prompt.c_str();req.negative_prompt=c.negative.c_str();
    req.duration_seconds=c.seconds;req.steps=c.steps;req.seed=c.seed;req.cfg_scale=1.f;
    req.residency=SA3_RESIDENCY_FRUGAL_V1;
    req.should_cancel=cancelled;req.on_progress=progress;req.callback_user=&token;
    sa3_result_v1 result{};result.size=sizeof result;api->result_init(&result);
    struct Release {const sa3_api_v1* api;sa3_result_v1* r;~Release(){api->result_free(r);}} release{api,&result};
    sa3_error_v1 e{};e.size=sizeof e;api->error_init(&e);
    const auto code=api->generate(runtime.ctx,&req,&result,&e);
    if(code!=SA3_STATUS_OK_V1){error=e.message;return {};}
    if(!s.current(epoch))return {};
    if(!result.samples||result.n_channels!=2||result.sample_rate!=rate||
       result.layout!=SA3_AUDIO_PLANAR_V1||result.n_samples!=uint64_t(std::llround(c.seconds*rate))||
       result.n_samples>uint64_t(max_seconds)*rate){error="Unexpected PCM format or duration";return {};}
    auto clip=std::make_shared<Clip>();clip->frames=size_t(result.n_samples);
    clip->pcm.assign(result.samples,result.samples+clip->frames*2);
    for(auto& sample:clip->pcm){if(!std::isfinite(sample)){error="Non-finite PCM";return {};}sample=std::clamp(sample,-1.f,1.f);}
    // Runtime and its model context are destroyed before playback begins.
    return clip;
}
bool play(State& s,const Clip& clip,uint64_t epoch){
    const auto start=std::chrono::steady_clock::now();
    const auto clock=os_gettime_ns();
    constexpr size_t block=rate/50;
    for(size_t i=0;i<clip.frames;i+=block){
        auto elapsed=std::chrono::nanoseconds(uint64_t(i)*1000000000ULL/rate);
        {std::unique_lock lock(s.mutex);s.cv.wait_until(lock,start+elapsed,[&]{return !s.current(epoch);});}
        if(!s.current(epoch)||std::chrono::steady_clock::now()>start+elapsed+250ms)return false;
        obs_source_audio out{};
        out.data[0]=reinterpret_cast<const uint8_t*>(clip.pcm.data()+i);
        out.data[1]=reinterpret_cast<const uint8_t*>(clip.pcm.data()+clip.frames+i);
        out.frames=uint32_t(std::min(block,clip.frames-i));out.format=AUDIO_FORMAT_FLOAT_PLANAR;
        out.speakers=SPEAKERS_STEREO;out.samples_per_sec=rate;out.timestamp=clock+uint64_t(elapsed.count());
        obs_source_output_audio(s.source,&out);
    }
    std::unique_lock lock(s.mutex);
    s.cv.wait_until(lock,start+std::chrono::nanoseconds(uint64_t(clip.frames)*1000000000ULL/rate),[&]{return !s.current(epoch);});
    return s.current(epoch);
}
void worker(State* s){
    while(true){
        Config c;Job job;uint64_t e;std::shared_ptr<const Clip> clip;
        {std::unique_lock lock(s->mutex);s->cv.wait(lock,[&]{return s->closing||s->pending!=Job::None;});
         if(s->closing)return;
         job=s->pending;s->pending=Job::None;e=s->epoch;c=s->config;clip=s->saved;}
        if(!s->current(e))continue;
        try{
            if(job==Job::Generate){
                ++s->started;s->progress=0;message(*s,e,"Generating");
                std::string error;
                clip=generate(*s,c,e,error);
                std::lock_guard lock(s->mutex);
                if(!s->current(e))continue;
                if(!clip){++s->failed;s->message=error.empty()?"Generation failed":error;blog(LOG_WARNING,"[Sound/Diffusion] %s",s->message.c_str());continue;}
                s->saved=clip;++s->completed;s->progress=100;s->message="Playing";
            }
            if(!clip)continue;
            message(*s,e,"Playing");
            bool ok=play(*s,*clip,e);
            std::lock_guard lock(s->mutex);
            if(s->current(e)){
                if(ok){++s->played;s->message="Ready to replay";}
                else{++s->failed;s->message="Playback deadline missed";}
            }
        }catch(const std::exception& ex){
            std::lock_guard lock(s->mutex);if(s->current(e)){++s->failed;s->message=ex.what();}
        }catch(...){
            std::lock_guard lock(s->mutex);if(s->current(e)){++s->failed;s->message="Unexpected diffusion error";}
        }
    }
}
void invalidate(State& s){++s.epoch;s.pending=Job::None;s.progress=0;s.cv.notify_all();}
void update(void* p,obs_data_t* d){
    auto& s=*static_cast<State*>(p);Config c;
    c.models=obs_data_get_string(d,"models_dir");c.prompt=obs_data_get_string(d,"prompt");
    c.negative=obs_data_get_string(d,"negative_prompt");c.device=obs_data_get_string(d,"device");
    c.steps=int(std::clamp<int64_t>(obs_data_get_int(d,"steps"),1,32));
    c.seed=std::max<int64_t>(-1,obs_data_get_int(d,"seed"));
    auto seconds=obs_data_get_double(d,"duration");c.seconds=std::isfinite(seconds)?std::clamp(seconds,1.,double(max_seconds)):12.;
    std::lock_guard lock(s.mutex);s.config=std::move(c);
}
void activate(void* p){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);s.active=true;s.enabled=obs_source_enabled(s.source);}
void deactivate(void* p){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);s.active=false;invalidate(s);s.message="Inactive";}
void enabled(void* p,calldata_t* d){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);s.enabled=calldata_bool(d,"enabled");if(!s.enabled){invalidate(s);s.message="Disabled";}}
bool queue(void* p,Job job){
    if(!p)return false;
    auto& s=*static_cast<State*>(p);
    if(job==Job::Generate){auto* d=obs_source_get_settings(s.source);update(&s,d);obs_data_release(d);}
    std::lock_guard lock(s.mutex);
    if(!s.active||!s.enabled||s.closing){s.message="Activate the source first";return false;}
    if(job==Job::Replay&&!s.saved){s.message="Generate a clip first";return false;}
    invalidate(s);s.pending=job;s.message="Queued";s.cv.notify_all();return false;
}
bool refresh(obs_properties_t*,obs_property_t*,void*){return true;}
bool make(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Generate);}
bool replay(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Replay);}
bool stop(obs_properties_t*,obs_property_t*,void* p){
    if(p){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);invalidate(s);s.message="Stopped";}return false;
}
void defaults(obs_data_t* d){
    obs_data_set_default_string(d,"models_dir","");obs_data_set_default_string(d,"device","auto");
    obs_data_set_default_string(d,"prompt","warm instrumental bedroom pop, soft drums, toy keys");
    obs_data_set_default_double(d,"duration",12);obs_data_set_default_int(d,"steps",8);obs_data_set_default_int(d,"seed",-1);
}
obs_properties_t* props(void* p){
    auto* d=obs_properties_create();
    obs_properties_add_path(d,"models_dir","Stable Audio 3 GGUF model directory",OBS_PATH_DIRECTORY,nullptr,nullptr);
    auto* device=obs_properties_add_list(d,"device","Compute device",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(device,"Auto (GPU preferred)","auto");obs_property_list_add_string(device,"CPU","cpu");
    obs_properties_add_text(d,"prompt","Prompt",OBS_TEXT_MULTILINE);
    obs_properties_add_text(d,"negative_prompt","Negative prompt",OBS_TEXT_DEFAULT);
    obs_properties_add_float_slider(d,"duration","Duration (seconds)",1,60,1);
    obs_properties_add_int_slider(d,"steps","Diffusion steps",1,32,1);
    obs_properties_add_int(d,"seed","Seed (-1 = random)",-1,2147483647,1);
    obs_properties_add_button2(d,"generate","Generate and play",make,p);
    obs_properties_add_button2(d,"replay","Replay last clip",replay,p);
    obs_properties_add_button2(d,"stop","Stop / cancel",stop,p);
    obs_properties_add_button2(d,"refresh_status","Refresh status",refresh,p);
    std::string status="Generate while active. Small-Music Q4_K_M; F16 encoder and autoencoder. Local inference only.";
    if(p){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);status+="\nStatus: "+s.message+" ("+std::to_string(s.progress.load())+"%)";}
    obs_properties_add_text(d,"note",status.c_str(),OBS_TEXT_INFO);
    return d;
}
void status(void* p,calldata_t* d){
    auto& s=*static_cast<State*>(p);
    calldata_set_int(d,"started",s.started);calldata_set_int(d,"completed",s.completed);
    calldata_set_int(d,"played",s.played);calldata_set_int(d,"failed",s.failed);calldata_set_int(d,"progress",s.progress);
    std::lock_guard lock(s.mutex);calldata_set_string(d,"message",s.message.c_str());
}
void* create(obs_data_t* d,obs_source_t* source){
    try{
        auto s=std::make_unique<State>(source);update(s.get(),d);
        s->worker=std::thread(worker,s.get());
        signal_handler_connect(obs_source_get_signal_handler(source),"enable",enabled,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),
          "void sound_diffusion_status_v1(out int started, out int completed, out int played, out int failed, out int progress, out string message)",status,s.get());
        return s.release();
    }catch(...){return nullptr;}
}
void destroy(void* p){
    auto* s=static_cast<State*>(p);
    signal_handler_disconnect(obs_source_get_signal_handler(s->source),"enable",enabled,s);
    {std::lock_guard lock(s->mutex);s->closing=true;invalidate(*s);}
    if(s->worker.joinable())s->worker.join();
    delete s;
}
const char* name(void*){return "LLL.sound Audio Diffusion";}
}
void register_sound_diffusion(){
    static obs_source_info i=[]{
        obs_source_info x{};x.id="sound_audio_diffusion_source";x.type=OBS_SOURCE_TYPE_INPUT;x.output_flags=OBS_SOURCE_AUDIO;
        x.get_name=name;x.create=create;x.destroy=destroy;x.get_defaults=defaults;x.get_properties=props;x.update=update;
        x.activate=activate;x.deactivate=deactivate;return x;
    }();
    obs_register_source(&i);
}
