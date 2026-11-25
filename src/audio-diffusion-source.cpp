#include <obs-module.h>
#include <util/platform.h>
#include <media-io/audio-io.h>
#include "interop/sound_signal_client.hpp"
#include <array>
#include <libsa3_v1.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <deque>
#include "framework/sa3_buffer_policy.hpp"
#include <fstream>
#include "framework/tts_wav.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef SOUND_WITH_SAME_MORPH
#include <spawn.h>
#include <sys/socket.h>
#include <poll.h>
#include "framework/same_resident.hpp"
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
extern char** environ;
#endif

namespace {
using namespace std::chrono_literals;
constexpr uint32_t rate=44100;
constexpr int max_seconds=60;
// Limit simultaneous diffusion jobs, including separate OBS sources.
std::timed_mutex inference_gate;
struct Config {
    std::string models,prompt,negative,device,variant="small-music",input_file,input_source,alternate_prompt,
        lora_a,lora_b,sound_channel,sight_channel;
    float lora_a_strength=1.f,lora_b_strength=1.f;
    std::string morph_a,morph_b,morph_effect="blend";float morph_mix=.5f,morph_position=.5f;int morph_radius=2,morph_grain=2;bool morph_ramp=false;
    bool automate_b=false,capture_muted=false,adaptive_buffer=false;
    double capture_seconds=6;
    float transform_noise=.5f;
    int steps=8;
    int64_t seed=-1;
    double seconds=12;
};
struct Clip {
    std::vector<float> pcm;
    size_t frames=0;
    uint32_t sample_rate=rate,source_channels=2;
};
enum class Job { None, Generate, Replay, Continuous, Transform, Extend, Morph };
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
    std::atomic<uint64_t> captured{0},sound_cues{0},sight_cues{0},automated{0};
    std::atomic<uint64_t> resident_starts{0},resident_requests{0},resident_graph_builds{0};
    std::atomic<bool> resident_active{false};
    // Session telemetry is protected by mutex. The cooperative finish flag
    // is also read by the native lookahead callback without taking a lock.
    std::atomic<bool> finish_requested{false};
    bool continuous_session=false,generating=false,adaptive_session=false,prefilling=false;
    unsigned queued_clips=0,target_clips=0;
    uint64_t reserve_frames=0,prefill_frames=0;
    uint64_t generation_ms=0,buffer_frames=0,session_frames=0,next_frames=0,underruns=0;
    const char* stage="idle";
    std::string message="Ready";
    bool current(uint64_t e) const {return !closing&&active&&enabled&&epoch==e;}
};
void message(State& s,uint64_t epoch,const std::string& value){
    std::lock_guard lock(s.mutex);if(s.current(epoch))s.message=value;
}
struct Cancel {
    State& state;uint64_t epoch;bool lookahead=false;
    bool current() const{return state.current(epoch)&&(!lookahead||!state.finish_requested);}
};
int SA3_CALL cancelled(void* p){
    auto& c=*static_cast<Cancel*>(p);return !c.current();
}
void SA3_CALL progress(void* p,const sa3_progress_v1* v){
    auto& c=*static_cast<Cancel*>(p);
    if(!v||!c.current())return;
    std::lock_guard lock(c.state.mutex);
    if(!c.current())return;
    if(std::isfinite(v->fraction))c.state.progress=int(std::clamp(v->fraction,0.f,1.f)*100);
    switch(v->stage){
    case SA3_PROGRESS_LOADING_V1:c.state.stage="loading";break;
    case SA3_PROGRESS_ENCODING_V1:c.state.stage="encoding";break;
    case SA3_PROGRESS_SAMPLING_V1:c.state.stage="sampling";break;
    case SA3_PROGRESS_DECODING_V1:c.state.stage="decoding";break;
    case SA3_PROGRESS_DONE_V1:c.state.stage="finalizing";break;
    default:c.state.stage="processing";break;
    }
}
struct GenerationStatus {
    Cancel token;
    const std::chrono::steady_clock::time_point begin=std::chrono::steady_clock::now();
    bool succeeded=false;
    explicit GenerationStatus(State& s,uint64_t e,bool lookahead):token{s,e,lookahead}{
        std::lock_guard lock(s.mutex);
        if(token.current()){s.generating=true;s.stage="queued";s.progress=0;}
    }
    ~GenerationStatus(){
        auto& s=token.state;
        std::lock_guard lock(s.mutex);
        if(s.current(token.epoch)){
            s.generating=false;
            if(succeeded)s.generation_ms=std::max<int64_t>(1,std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-begin).count());
            s.stage=succeeded?"ready":"idle";
        }
    }
};
struct SessionEnd {
    State& s;uint64_t epoch;
    ~SessionEnd(){
        std::lock_guard lock(s.mutex);
        if(s.current(epoch)){
            s.continuous_session=false;s.finish_requested=false;s.generating=false;
            s.buffer_frames=s.next_frames=0;s.stage="idle";
            s.adaptive_session=s.prefilling=false;s.queued_clips=s.target_clips=0;s.reserve_frames=0;
        }
    }
};
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
        cfg.models_dir=c.models.c_str();cfg.variant=c.variant.c_str();cfg.dit_encoding="q4_k_m";
        cfg.text_encoder_encoding="f16";cfg.autoencoder_encoding="f16";
        cfg.device=c.device=="cpu"?"cpu":nullptr;cfg.cpu_threads=8;
        sa3_error_v1 e{};e.size=sizeof e;api->error_init(&e);
        if(api->context_create(&cfg,&ctx,&e)!=SA3_STATUS_OK_V1){error=e.message;return false;}
        return true;
    }
};
struct SightMotionV1 {
    uint32_t abi_version=1,struct_size=sizeof(SightMotionV1),source_width=0,source_height=0;
    uint64_t sequence=0;
    float energy=0;
    uint32_t reserved=0;
};
static_assert(sizeof(SightMotionV1)==32,"Sight motion ABI mismatch");
bool sight_filter(obs_source_t* source){
    const auto* id=source?obs_source_get_unversioned_id(source):nullptr;
    return id&&!std::strcmp(id,"sight_u2net_filter");
}
bool read_sight_motion(const std::string& channel,float& energy){
    const auto sep=channel.find('/');
    if(sep==std::string::npos||channel.size()>160)return false;
    auto* parent=obs_get_source_by_uuid(channel.substr(0,sep).c_str());
    if(!parent)return false;
    struct Find {std::string uuid;obs_source_t* filter=nullptr;} find{channel.substr(sep+1)};
    if(!obs_source_removed(parent)&&obs_source_enabled(parent))
        obs_source_enum_filters(parent,[](obs_source_t*,obs_source_t* filter,void* opaque){
            auto& f=*static_cast<Find*>(opaque);
            if(!f.filter&&sight_filter(filter)&&obs_source_enabled(filter)&&!obs_source_removed(filter)&&
               obs_source_get_uuid(filter)&&f.uuid==obs_source_get_uuid(filter))f.filter=obs_source_get_ref(filter);
        },&find);
    obs_source_release(parent);
    if(!find.filter)return false;
    SightMotionV1 result;
    calldata_t args{};calldata_set_int(&args,"max_age_ms",750);calldata_set_ptr(&args,"snapshot",&result);
    bool ok=proc_handler_call(obs_source_get_proc_handler(find.filter),"sight_motion_read_v1",&args)&&calldata_bool(&args,"available");
    calldata_free(&args);
    const auto w=obs_source_get_base_width(find.filter),h=obs_source_get_base_height(find.filter);
    obs_source_release(find.filter);
    if(!ok||result.abi_version!=1||result.struct_size!=sizeof(result)||!result.sequence||!w||!h||
       result.source_width!=w||result.source_height!=h||!std::isfinite(result.energy)||result.energy<0||result.energy>1)return false;
    energy=result.energy;return true;
}
std::string condition_prompt(State& s,const Config& c){
    std::string prompt=c.prompt;
    if(!c.sound_channel.empty()){
        // A final transcript or classification label is an optional local
        // conditioning cue, never an instruction to a separate LLM.
        calldata_t d{};sound_signals::read(c.sound_channel,"final","",&d);
        if(calldata_bool(&d,"available")&&calldata_bool(&d,"final")){
            auto* raw=calldata_string(&d,"text");
            if(raw&&*raw){prompt+=", spoken scene context: "+std::string(raw).substr(0,256);++s.sound_cues;}
        }else{
            calldata_free(&d);d={};
            sound_signals::read(c.sound_channel,"latest","",&d);
            auto* kind=calldata_string(&d,"kind"),*label=calldata_string(&d,"label");
            if(calldata_bool(&d,"available")&&kind&&!std::strcmp(kind,"classification")&&label&&*label&&
               calldata_float(&d,"score")>=.4){
                prompt+=", ambient sound: "+std::string(label).substr(0,128);++s.sound_cues;
            }
        }
        calldata_free(&d);
    }
    float motion=0;
    if(!c.sight_channel.empty()&&read_sight_motion(c.sight_channel,motion)){
        prompt+=motion>.08f?", energetic rhythm and dynamic arrangement":motion>.02f?", gently moving rhythm":", restrained calm arrangement";
        ++s.sight_cues;
    }
    return prompt;
}
std::shared_ptr<Clip> generate(State& s,const Config& c,uint64_t epoch,std::string& error,
                               const Clip* input=nullptr,bool extend=false,bool continuous=false){
    if(continuous&&(!input||input->frames<rate/2)){error="Invalid continuation context";return {};}
    if(c.models.empty()||c.prompt.empty()){error="Choose a model directory and enter a prompt";return {};}
    if(c.prompt.size()>7500||c.negative.size()>8192||c.alternate_prompt.size()>8192){error="Prompt exceeds 8192 bytes";return {};}
    GenerationStatus generation(s,epoch,continuous);
    if(!generation.token.current())return {};
    // Snapshot cues when scheduling the segment, before model load or GPU wait.
    const auto conditioned=condition_prompt(s,c);
    std::unique_lock<std::timed_mutex> gate(inference_gate,std::defer_lock);
    while(generation.token.current()&&!gate.try_lock_for(50ms)){}
    if(!generation.token.current()||!gate.owns_lock())return {};
    {std::lock_guard lock(s.mutex);if(generation.token.current())s.stage="loading";}
    Runtime runtime;
    if(!runtime.open(c,error)||!generation.token.current())return {};
    const auto* api=runtime.api;
    auto& token=generation.token;
    sa3_request_v1 req{};req.size=sizeof req;api->request_init(&req);
    req.prompt=conditioned.c_str();req.negative_prompt=c.negative.c_str();
    req.duration_seconds=c.seconds;req.steps=c.steps;req.seed=c.seed;req.cfg_scale=1.f;
    req.residency=SA3_RESIDENCY_FRUGAL_V1;
    if(input){
        req.operation=extend?SA3_OPERATION_CONTINUE_V1:SA3_OPERATION_TRANSFORM_V1;
        req.input_audio.size=sizeof(req.input_audio);
        req.input_audio.samples=input->pcm.data();req.input_audio.n_samples=input->frames;
        req.input_audio.n_channels=2;req.input_audio.sample_rate=input->sample_rate;req.input_audio.layout=SA3_AUDIO_PLANAR_V1;
        req.transform_noise_level=c.transform_noise;
        req.encode_chunk_size=128;req.encode_overlap=32;req.decode_chunk_size=128;req.decode_overlap=32;
        if(continuous){
            // Hold back more than the runtime's 200 ms mask + 30 ms splice.
            // Never renormalize already emitted audio.
            req.continuation.gain_match=0;req.loudness.peak_normalize=0;req.loudness.limiter=0;
        }
    }
    const uint64_t input_frames=input?uint64_t(std::llround(double(input->frames)*rate/input->sample_rate)):0;
    const uint64_t expected=input?(extend?input_frames+uint64_t(std::llround(c.seconds*rate)):input_frames):
        uint64_t(std::llround(c.seconds*rate));
    if(expected>uint64_t(max_seconds)*rate){error="Result would exceed 60 seconds";return {};}
    std::array<sa3_adapter_v1,2> adapters{};uint32_t adapter_count=0;
    sa3_adapter_v1 lane_adapter{};sa3_lane_v1 lane{};float envelope[4]{};
    sa3_error_v1 adapter_error{};adapter_error.size=sizeof adapter_error;api->error_init(&adapter_error);
    const bool has_extended_api=api->size>=offsetof(sa3_api_v1,adapter_info)+sizeof(api->adapter_info);
    auto adapter=[&](const std::string& path,float strength,sa3_adapter_v1& out,bool for_lane){
        std::error_code ec;
        if(!has_extended_api||!api->adapter_info||!api->adapter_init||
           std::filesystem::path(path).extension()!=".gguf"||!std::filesystem::is_regular_file(path,ec)){
            error="Choose a compatible local GGUF LoRA";return false;
        }
        sa3_adapter_info_v1 info{};info.size=sizeof info;
        if(api->adapter_info(path.c_str(),&info,&adapter_error)!=SA3_STATUS_OK_V1){
            error=adapter_error.message;return false;
        }
        if(for_lane&&!info.supports_lanes){error="This LoRA does not support automation lanes";return false;}
        out.size=sizeof out;api->adapter_init(&out);out.path_or_name=path.c_str();out.strength=strength;
        return true;
    };
    if(!c.lora_a.empty()&&c.lora_a_strength!=0){
        if(!adapter(c.lora_a,c.lora_a_strength,adapters[adapter_count],false))return {};
        ++adapter_count;
    }
    const bool ramp_adapter=c.automate_b&&!c.lora_b.empty()&&c.lora_b_strength!=0;
    if(!c.lora_b.empty()&&c.lora_b_strength!=0){
        if(!adapter(c.lora_b,c.lora_b_strength,ramp_adapter?lane_adapter:adapters[adapter_count],ramp_adapter))return {};
        if(!ramp_adapter)++adapter_count;
    }
    if(adapter_count){req.adapters=adapters.data();req.adapter_count=adapter_count;req.adapter_stride=sizeof(sa3_adapter_v1);}
    if(!c.alternate_prompt.empty()||ramp_adapter){
        if(!has_extended_api||!api->lane_init){error="SA3 runtime does not support automation";return {};}
        lane.size=sizeof lane;api->lane_init(&lane);
        lane.prompt=c.alternate_prompt.empty()?nullptr:c.alternate_prompt.c_str();
        if(ramp_adapter){lane.adapters=&lane_adapter;lane.adapter_count=1;lane.adapter_stride=sizeof lane_adapter;}
        envelope[0]=extend?float(input_frames)/rate:0.f;
        envelope[1]=0.f;envelope[2]=float(expected)/rate;envelope[3]=1.f;
        lane.envelope=envelope;lane.envelope_point_count=2;
        req.lanes=&lane;req.lane_count=1;req.lane_stride=sizeof lane;
    }
    req.should_cancel=cancelled;req.on_progress=progress;req.callback_user=&token;
    sa3_result_v1 result{};result.size=sizeof result;api->result_init(&result);
    struct Release {const sa3_api_v1* api;sa3_result_v1* r;~Release(){api->result_free(r);}} release{api,&result};
    sa3_error_v1 e{};e.size=sizeof e;api->error_init(&e);
    const auto code=api->generate(runtime.ctx,&req,&result,&e);
    if(code!=SA3_STATUS_OK_V1){error=e.message;return {};}
    if(!token.current())return {};
    if(!result.samples||result.n_channels!=2||result.sample_rate!=rate||
       result.layout!=SA3_AUDIO_PLANAR_V1||result.n_samples!=expected||
       result.n_samples>uint64_t(max_seconds)*rate){error="Unexpected PCM format or duration";return {};}
    if(extend&&!result.splice_applied){error="Continuation did not splice source";return {};}
    auto clip=std::make_shared<Clip>();clip->frames=size_t(result.n_samples);
    clip->pcm.assign(result.samples,result.samples+clip->frames*2);
    float peak=0;
    for(float sample:clip->pcm){if(!std::isfinite(sample)){error="Non-finite PCM";return {};}peak=std::max(peak,std::abs(sample));}
    // Preserve headroom for OBS's output resampler. Scale uniformly rather
    // than hard-clipping the generated waveform.
    if(peak>.75f)for(auto& sample:clip->pcm)sample*=.75f/peak;
    if(req.lane_count)++s.automated;
    generation.succeeded=true;
    // Runtime and its model context are destroyed before playback begins.
    return clip;
}
// Read a bounded, local PCM16 stereo/mono WAV at the model rate. Do not
// pass unbounded or unsupported files into the native inference engine.
std::shared_ptr<Clip> read_wav(const std::string& path,std::string& error){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f||f.tellg()<44||f.tellg()>std::streamoff(48000*max_seconds*4+65536)){error="Invalid or oversized WAV";return {};}
    std::vector<uint8_t> b(size_t(f.tellg()));f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(b.data()),b.size())||std::memcmp(b.data(),"RIFF",4)||
       std::memcmp(b.data()+8,"WAVE",4)||stts::wav_u32(b.data()+4)!=b.size()-8){error="Invalid RIFF WAV";return {};}
    unsigned channels=0,wav_rate=0;const uint8_t* data=nullptr;size_t bytes=0;
    for(size_t pos=12;pos+8<=b.size();){
        auto* tag=b.data()+pos;auto n=stts::wav_u32(tag+4);pos+=8;
        if(n>b.size()-pos){error="Truncated WAV";return {};}
        if(!std::memcmp(tag,"fmt ",4)){
            if(channels||n<16||stts::wav_u16(b.data()+pos)!=1||stts::wav_u16(b.data()+pos+14)!=16||
               (stts::wav_u32(b.data()+pos+4)!=44100&&stts::wav_u32(b.data()+pos+4)!=48000)){error="Use 44.1 or 48 kHz PCM16 WAV";return {};}
            wav_rate=stts::wav_u32(b.data()+pos+4);
            channels=stts::wav_u16(b.data()+pos+2);
            if((channels!=1&&channels!=2)||stts::wav_u16(b.data()+pos+12)!=channels*2||
               stts::wav_u32(b.data()+pos+8)!=wav_rate*channels*2){error="Invalid WAV channel layout";return {};}
        }else if(!std::memcmp(tag,"data",4)){
            if(data){error="Duplicate WAV data";return {};}data=b.data()+pos;bytes=n;
        }
        pos+=n;if(n%2){if(pos==b.size()){error="Truncated WAV padding";return {};}++pos;}
    }
    if(!channels||!data||!bytes||bytes%(channels*2)||bytes/(channels*2)>wav_rate*max_seconds){error="Invalid WAV data";return {};}
    auto clip=std::make_shared<Clip>();clip->frames=bytes/(channels*2);clip->sample_rate=wav_rate;clip->source_channels=channels;clip->pcm.resize(clip->frames*2);
    for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<clip->frames;++i){
        const auto v=stts::wav_u16(data+2*(i*channels+std::min(ch,size_t(channels-1))));
        clip->pcm[ch*clip->frames+i]=float(int(v)-(v>=32768?65536:0))/32768.f;
    }
    return clip;
}
// Explicit bounded snapshot capture: keep the input source dry and submit
// the transformed result as this source. No native inference in the callback.
bool capture_source(obs_source_t* source){
    const auto* id=source?obs_source_get_unversioned_id(source):nullptr;
    return id&&std::strcmp(id,"sound_audio_diffusion_source")&&
        obs_source_get_type(source)==OBS_SOURCE_TYPE_INPUT&&(obs_source_get_output_flags(source)&OBS_SOURCE_AUDIO);
}
struct CaptureJob {
    Clip& clip;
    unsigned channels;
    bool muted;
    std::atomic<size_t> written{0};
    std::atomic<bool> invalid{false};
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
};
void capture_audio(void* opaque,obs_source_t*,const audio_data* audio,bool muted){
    auto& job=*static_cast<CaptureJob*>(opaque);
    if((muted&&!job.muted)||!audio||!audio->data[0]||job.invalid||job.busy.test_and_set(std::memory_order_acquire))return;
    struct Unlock {std::atomic_flag& flag;~Unlock(){flag.clear(std::memory_order_release);}} unlock{job.busy};
    const auto start=job.written.load(std::memory_order_relaxed);
    const auto n=std::min<size_t>(audio->frames,job.clip.frames-start);
    if(job.channels==2&&!audio->data[1]){job.invalid=true;return;}
    for(size_t ch=0;ch<2;++ch){
        auto* in=reinterpret_cast<const float*>(audio->data[job.channels==1?0:ch]);
        auto* out=job.clip.pcm.data()+ch*job.clip.frames+start;
        for(size_t i=0;i<n;++i){
            if(!std::isfinite(in[i])){job.invalid=true;return;}
            out[i]=std::clamp(in[i],-1.f,1.f);
        }
    }
    job.written.store(start+n,std::memory_order_release);
}
std::shared_ptr<Clip> capture_input(State& s,const Config& c,uint64_t epoch,std::string& error){
    auto* source=obs_get_source_by_uuid(c.input_source.c_str());
    if(!capture_source(source)||source==s.source||obs_source_removed(source)||!obs_source_enabled(source)){
        if(source)obs_source_release(source);
        error="Input source unavailable";return {};
    }
    struct SourceRef {obs_source_t* s;~SourceRef(){obs_source_release(s);}} ref{source};
    auto* audio=obs_get_audio();
    const auto sample_rate=audio?audio_output_get_sample_rate(audio):0;
    const auto channels=audio?audio_output_get_channels(audio):0;
    if((sample_rate!=44100&&sample_rate!=48000)||(channels!=1&&channels!=2)){
        error="Live capture requires mono/stereo at 44.1 or 48 kHz";return {};
    }
    auto clip=std::make_shared<Clip>();clip->sample_rate=sample_rate;
    clip->frames=size_t(std::llround(c.capture_seconds*sample_rate));clip->pcm.resize(clip->frames*2);
    CaptureJob job{*clip,unsigned(channels),c.capture_muted};
    {std::lock_guard lock(s.mutex);if(!s.current(epoch))return {};s.stage="capturing";s.message="Capturing input";}
    obs_source_add_audio_capture_callback(source,capture_audio,&job);
    struct Remove {obs_source_t* source;CaptureJob* job;~Remove(){obs_source_remove_audio_capture_callback(source,capture_audio,job);}} remove{source,&job};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(int(c.capture_seconds*1000))+5s;
    while(s.current(epoch)&&job.written.load(std::memory_order_acquire)<clip->frames&&!job.invalid&&
          !obs_source_removed(source)&&obs_source_enabled(source)&&std::chrono::steady_clock::now()<deadline){
        std::unique_lock lock(s.mutex);s.cv.wait_for(lock,20ms,[&]{return !s.current(epoch);});
    }
    if(!s.current(epoch))return {};
    if(job.invalid||job.written.load(std::memory_order_acquire)!=clip->frames){error="Incomplete or invalid source audio capture";return {};}
    ++s.captured;
    return clip;
}
struct PlayClock {
    const std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    const uint64_t clock=os_gettime_ns();
    uint64_t emitted=0;
    auto elapsed()const{return std::chrono::seconds(emitted/rate)+std::chrono::nanoseconds((emitted%rate)*1000000000ULL/rate);}
};
bool play_range(State& s,const Clip& clip,size_t count,PlayClock& clock,uint64_t epoch,size_t offset=0){
    constexpr size_t block=rate/50;
    if(offset>clip.frames||count>clip.frames-offset)return false;
    {std::lock_guard lock(s.mutex);if(!s.current(epoch))return false;s.buffer_frames=clip.frames-offset;}
    for(size_t i=0;i<count;i+=block){
        auto elapsed=clock.elapsed();
        {std::unique_lock lock(s.mutex);s.cv.wait_until(lock,clock.start+elapsed,[&]{return !s.current(epoch);});}
        if(!s.current(epoch)||std::chrono::steady_clock::now()>clock.start+elapsed+250ms)return false;
        obs_source_audio out{};
        out.data[0]=reinterpret_cast<const uint8_t*>(clip.pcm.data()+offset+i);
        out.data[1]=reinterpret_cast<const uint8_t*>(clip.pcm.data()+clip.frames+offset+i);
        out.frames=uint32_t(std::min(block,count-i));out.format=AUDIO_FORMAT_FLOAT_PLANAR;
        out.speakers=SPEAKERS_STEREO;out.samples_per_sec=rate;out.timestamp=clock.clock+uint64_t(elapsed.count());
        obs_source_output_audio(s.source,&out);clock.emitted+=out.frames;
        {std::lock_guard lock(s.mutex);if(s.current(epoch)){
            s.session_frames+=out.frames;s.buffer_frames=clip.frames-offset-i-out.frames;
        }}
    }
    std::unique_lock lock(s.mutex);
    s.cv.wait_until(lock,clock.start+clock.elapsed(),[&]{return !s.current(epoch);});
    return s.current(epoch);
}
bool play(State& s,const Clip& clip,uint64_t epoch){PlayClock clock;return play_range(s,clip,clip.frames,clock,epoch);}
constexpr size_t held=rate*35/100; // 350 ms unplayed splice region
std::shared_ptr<Clip> tail(const Clip& clip,size_t frames){
    auto out=std::make_shared<Clip>();out->frames=std::min(frames,clip.frames);
    out->pcm.resize(out->frames*2);
    for(size_t ch=0;ch<2;++ch)
        std::copy_n(clip.pcm.data()+ch*clip.frames+clip.frames-out->frames,out->frames,out->pcm.data()+ch*out->frames);
    return out;
}
struct Next {std::shared_ptr<Clip> clip;std::string error;};
Next continuation(State& s,Config c,uint64_t epoch,std::shared_ptr<const Clip> current,uint64_t segment,bool publish_next=true){
    Next next;
    try {
        auto context=tail(*current,rate*4);
        // With 4 seconds of context, cap each new segment at 56 seconds.
        c.seconds=std::clamp(c.seconds,6.,56.);
        if(c.seed>=0)c.seed=(c.seed+int64_t(segment%2147483647))%2147483647;
        {std::lock_guard lock(s.mutex);if(!s.current(epoch)||s.finish_requested)return next;++s.started;s.progress=0;}
        auto full=generate(s,c,epoch,next.error,context.get(),true,true);
        if(!full)return next;
        auto output=tail(*full,full->frames-context->frames+held);
        // Preserve the start of the held region even if the native safety
        // scaler adjusted the returned prefix. Blend into its real splice.
        constexpr size_t blend=rate/50;
        for(size_t ch=0;ch<2;++ch)for(size_t i=0;i<blend;++i){
            float mix=float(i)/float(blend-1);
            const float old=current->pcm[ch*current->frames+current->frames-held+i];
            auto& x=output->pcm[ch*output->frames+i];x=old*(1-mix)+x*mix;
        }
        next.clip=std::move(output);
        {std::lock_guard lock(s.mutex);if(publish_next&&s.current(epoch)&&!s.finish_requested)s.next_frames=next.clip->frames;}
        return next;
    }catch(const std::exception& e){next.error=e.what();return next;}
    catch(...){next.error="Continuation failed";return next;}
}
// Caller holds s.mutex. Family and device stay fixed for the session.
void live_config(Config& c,const Config& live){
    c.prompt=live.prompt;c.negative=live.negative;c.seconds=live.seconds;
    c.steps=live.steps;c.seed=live.seed;
    c.alternate_prompt=live.alternate_prompt;c.lora_a=live.lora_a;c.lora_b=live.lora_b;
    c.lora_a_strength=live.lora_a_strength;c.lora_b_strength=live.lora_b_strength;
    c.automate_b=live.automate_b;c.sound_channel=live.sound_channel;c.sight_channel=live.sight_channel;
}
void buffered_play(State& s,Config c,uint64_t epoch,std::shared_ptr<Clip> clip){
    // One producer follows the last generated clip, never the current
    // playback position. The queue is bounded, ordered and mutex-protected.
    std::deque<std::shared_ptr<Clip>> queue;
    sa3buffer::Policy policy;
    bool producer_done=false;
    std::string producer_error;
    {
        std::lock_guard lock(s.mutex);
        if(!s.current(epoch))return;
        policy.observe(s.generation_ms);
        s.target_clips=policy.target(uint64_t(c.seconds*1000));
        s.prefilling=true;s.message="Preparing playback reserve";
    }
    auto publish=[&]{
        s.queued_clips=unsigned(queue.size());
        s.next_frames=queue.empty()?0:queue.front()->frames;
        s.reserve_frames=0;
        for(auto& item:queue)s.reserve_frames+=item->frames-held;
    };
    auto producer=std::async(std::launch::async,[&,last=clip,config=c]() mutable {
        uint64_t segment=0;
        try{
            while(true){
                {
                    std::unique_lock lock(s.mutex);
                    s.cv.wait(lock,[&]{return !s.current(epoch)||s.finish_requested||queue.size()<s.target_clips;});
                    if(!s.current(epoch)||s.finish_requested)break;
                    live_config(config,s.config);
                }
                auto next=continuation(s,config,epoch,last,++segment,false);
                std::lock_guard lock(s.mutex);
                if(!s.current(epoch)||s.finish_requested)break;
                if(!next.clip){producer_error=next.error.empty()?"Continuation failed":next.error;break;}
                last=next.clip;queue.push_back(std::move(next.clip));
                policy.observe(s.generation_ms);
                s.target_clips=policy.target((last->frames-held)*1000/rate);
                publish();s.cv.notify_all();
            }
        }catch(...){
            std::lock_guard lock(s.mutex);producer_error="Buffer generation failed";
        }
        std::lock_guard lock(s.mutex);producer_done=true;s.cv.notify_all();
    });
    // Cancel and wake the producer before the future destructor joins it,
    // including exceptional exits and superseded commands.
    struct CancelProducer {
        State& s;uint64_t epoch;
        ~CancelProducer(){std::lock_guard lock(s.mutex);if(s.current(epoch))s.finish_requested=true;s.cv.notify_all();}
    } cancel{s,epoch};
    {
        std::unique_lock lock(s.mutex);
        s.cv.wait(lock,[&]{return !s.current(epoch)||s.finish_requested||producer_done||queue.size()>=s.target_clips;});
        if(!s.current(epoch))return;
        s.prefilling=false;s.prefill_frames=s.reserve_frames;
    }
    PlayClock clock;
    while(s.current(epoch)){
        bool finish;
        {
            std::lock_guard lock(s.mutex);
            if(!s.current(epoch))return;
            s.saved=clip;++s.completed;
            finish=s.finish_requested;
            s.message=finish?"Finishing current segment":"Playing / buffering";
        }
        const auto count=finish?clip->frames:clip->frames-held;
        if(!play_range(s,*clip,count,clock,epoch)){
            std::lock_guard lock(s.mutex);
            if(s.current(epoch)){++s.failed;++s.underruns;s.message="Playback deadline missed";}
            return;
        }
        enum class End { Finish, Miss, Failure };
        End reason=End::Finish;
        std::string error;
        {
            std::lock_guard lock(s.mutex);
            if(!s.current(epoch))return;
            if(!s.finish_requested&&!queue.empty()){
                ++s.played;clip=std::move(queue.front());queue.pop_front();publish();s.cv.notify_all();
                continue;
            }
            if(!s.finish_requested){
                reason=producer_done?End::Failure:End::Miss;
                error=producer_error;
            }
            s.finish_requested=true;queue.clear();publish();s.cv.notify_all();
        }
        bool tail_sent=finish||play_range(s,*clip,held,clock,epoch,clip->frames-held);
        std::lock_guard lock(s.mutex);
        if(s.current(epoch)){
            if(!tail_sent){++s.failed;++s.underruns;s.message="Playback deadline missed";}
            else if(reason==End::Finish){++s.played;s.message="Finished segment";}
            else if(reason==End::Miss){++s.failed;++s.underruns;s.message="Adaptive reserve exhausted; increase duration";}
            else{++s.failed;s.message=error.empty()?"Continuation failed":error;}
        }
        return;
    }
}
void continuous(State& s,Config c,uint64_t epoch){
    // Continuous mode is music only. SFX remains explicit, bounded generation.
    if(c.variant!="small-music"){
        std::lock_guard lock(s.mutex);if(s.current(epoch)){++s.failed;s.message="Continuous mode requires Small Music";}
        return;
    }
    c.seconds=std::clamp(c.seconds,6.,56.);
    {std::lock_guard lock(s.mutex);if(!s.current(epoch))return;++s.started;s.progress=0;s.message="Generating first segment";}
    std::string error;auto clip=generate(s,c,epoch,error);
    if(!clip){
        std::lock_guard lock(s.mutex);if(s.current(epoch)){++s.failed;s.message=error.empty()?"Initial generation failed":error;}
        return;
    }
    if(c.adaptive_buffer){buffered_play(s,c,epoch,std::move(clip));return;}
    PlayClock clock;uint64_t segment=0;
    while(s.current(epoch)){
        {
            std::lock_guard lock(s.mutex);
            if(!s.current(epoch))return;
            s.saved=clip;++s.completed;s.next_frames=0;
            s.message=s.finish_requested?"Finishing current segment":"Playing / generating next";
            // Read live prompt and sampling controls at each segment boundary.
            // Model/device/variant are fixed until restart.
            live_config(c,s.config);
        }
        if(s.finish_requested){
            const bool sent=play_range(s,*clip,clip->frames,clock,epoch);
            std::lock_guard lock(s.mutex);
            if(s.current(epoch)){
                if(sent){++s.played;s.message="Finished segment";}
                else{++s.failed;++s.underruns;s.message="Playback deadline missed";}
            }
            return;
        }
        auto future=std::async(std::launch::async,continuation,std::ref(s),c,epoch,std::shared_ptr<const Clip>(clip),++segment,true);
        bool sent=play_range(s,*clip,clip->frames-held,clock,epoch);
        if(!sent){
            std::lock_guard lock(s.mutex);if(s.current(epoch)){s.finish_requested=true;++s.failed;++s.underruns;s.message="Playback deadline missed";s.cv.notify_all();}
            return; // joins producer after releasing the mutex
        }
        if(!s.current(epoch))return;
        Next next;
        const bool ready=future.wait_for(0ms)==std::future_status::ready;
        if(ready)next=future.get();
        enum class Boundary { Handoff, Finish, Miss, Failure };
        Boundary decision;
        {
            std::lock_guard lock(s.mutex);
            if(!s.current(epoch))return;
            if(s.finish_requested)decision=Boundary::Finish;
            else if(!ready)decision=Boundary::Miss;
            else if(!next.clip)decision=Boundary::Failure;
            else decision=Boundary::Handoff;
            if(decision==Boundary::Handoff){
                ++s.played;clip=std::move(next.clip);
                s.next_frames=0;
            }else{
                s.finish_requested=true;s.next_frames=0;
            }
        }
        if(decision==Boundary::Handoff)continue;
        // Cancel pending lookahead, but play the original tail without a gap.
        // The future joins before session cleanup; new commands still cancel
        // this epoch immediately.
        const bool tail_sent=play_range(s,*clip,held,clock,epoch,clip->frames-held);
        {
            std::lock_guard lock(s.mutex);
            if(!s.current(epoch))return;
            if(!tail_sent){++s.failed;++s.underruns;s.message="Playback deadline missed";}
            else if(decision==Boundary::Finish){++s.played;s.message="Finished segment";}
            else if(decision==Boundary::Miss){++s.failed;++s.underruns;s.message="Continuation missed lookahead deadline; increase duration";}
            else{++s.failed;s.message=next.error.empty()?"Continuation failed":next.error;}
        }
        return;
    }
}
#ifdef SOUND_WITH_SAME_MORPH
// One retained autoencoder across all sources. Contending sources retain
// the previous one-shot path instead of creating unbounded warm contexts.
std::mutex same_residency_gate;
class ResidentWorker {
    State& state;
    std::unique_lock<std::mutex> slot{same_residency_gate,std::defer_lock};
    int fd=-1;
    pid_t pid=0;
    std::string model,device;
    uint32_t seq=0;
public:
    uint64_t epoch=0;
    explicit ResidentWorker(State& s):state(s){}
    ~ResidentWorker(){close();}
    bool active()const{return pid>0;}
    void close() noexcept {
        if(fd>=0){::close(fd);fd=-1;}
        if(pid>0){
            int status=0;pid_t r;
            do{r=waitpid(pid,&status,WNOHANG);}while(r<0&&errno==EINTR);
            if(r==0){kill(pid,SIGKILL);do{r=waitpid(pid,&status,0);}while(r<0&&errno==EINTR);}
            pid=0;
        }
        state.resident_active=false;seq=0;
        if(slot.owns_lock())slot.unlock();
    }
    bool start(const std::string& exe,const std::string& path,const std::string& backend){
        if(active()&&(path!=model||backend!=device||seq==UINT32_MAX))close();
        if(active())return true;
        if(!slot.try_lock())return false;
        try{
            model=path;device=backend;
            int fds[2];
            if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fds))throw std::runtime_error("Cannot open SAME socket");
            fd=fds[0];
            struct ChildFd{int fd;~ChildFd(){if(fd>=0)::close(fd);}} child{fds[1]};
            // Do not accidentally overwrite one of the socket endpoints
            // while arranging the child's standard descriptors.
            if(child.fd<3){const int dup=fcntl(child.fd,F_DUPFD_CLOEXEC,3);if(dup<0)throw std::runtime_error("Cannot duplicate SAME socket");::close(child.fd);child.fd=dup;}
            if(fd<3){const int dup=fcntl(fd,F_DUPFD_CLOEXEC,3);if(dup<0)throw std::runtime_error("Cannot duplicate SAME socket");::close(fd);fd=dup;}
            posix_spawn_file_actions_t actions;
            if(posix_spawn_file_actions_init(&actions))throw std::runtime_error("Cannot initialize SAME worker");
            struct Guard{posix_spawn_file_actions_t* p;~Guard(){posix_spawn_file_actions_destroy(p);}} guard{&actions};
            if(posix_spawn_file_actions_adddup2(&actions,child.fd,STDIN_FILENO)||
               posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0)||
               posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO))
                throw std::runtime_error("Cannot configure SAME worker");
            std::array<std::string,4> args={exe,"--serve",path,backend};
            std::array<char*,5> argv{};for(size_t i=0;i<args.size();++i)argv[i]=args[i].data();
            pid_t spawned=0;
            if(posix_spawn(&spawned,exe.c_str(),&actions,nullptr,argv.data(),environ))
                throw std::runtime_error("Cannot start resident SAME worker");
            pid=spawned;state.resident_active=true;++state.resident_starts;
            state.resident_graph_builds=0;return true;
        }catch(...){close();throw;}
    }
    bool transfer(void* bytes,size_t size,bool writing,const Cancel& token,std::chrono::steady_clock::time_point deadline){
        auto* p=static_cast<char*>(bytes);
        while(size&&token.current()){
            const auto now=std::chrono::steady_clock::now();if(now>=deadline)return false;
            pollfd event{fd,short(writing?POLLOUT:POLLIN),0};
            const int ready=poll(&event,1,20);
            if(ready<0){if(errno==EINTR)continue;return false;}
            if(!ready)continue;
            if(!(event.revents&(writing?POLLOUT:POLLIN)))return false;
            const auto n=writing?send(fd,p,size,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,p,size,MSG_DONTWAIT);
            if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
            if(n<=0)return false;
            p+=n;size-=size_t(n);
        }
        return size==0&&token.current();
    }
    std::shared_ptr<Clip> request(Clip& input,const Config& c,const Cancel& token){
        using namespace same_resident;
        try{
            Request r{magic,++seq,uint32_t(input.frames),c.morph_effect=="freeze"?1u:c.morph_effect=="smear"?2u:3u,
                c.morph_mix,c.morph_position,uint32_t(c.morph_radius),uint32_t(c.morph_grain)};
            Reply reply{};
            const auto deadline=std::chrono::steady_clock::now()+45s;
            if(!transfer(&r,sizeof r,true,token,deadline)||
               !transfer(input.pcm.data(),input.pcm.size()*sizeof(float),true,token,deadline)||
               !transfer(&reply,sizeof reply,false,token,deadline)||
               reply.magic!=magic||reply.sequence!=r.sequence||reply.status!=0||reply.frames!=r.frames||!reply.builds)
                throw std::runtime_error("Resident SAME worker cancelled, rejected input or failed");
            auto out=std::make_shared<Clip>();out->frames=reply.frames;out->pcm.resize(size_t(reply.frames)*2);
            if(!transfer(out->pcm.data(),out->pcm.size()*sizeof(float),false,token,deadline)||
               !std::all_of(out->pcm.begin(),out->pcm.end(),[](float v){return std::isfinite(v)&&std::abs(v)<=.751f;}))
                throw std::runtime_error("Invalid resident SAME output");
            state.resident_graph_builds=reply.builds;++state.resident_requests;
            epoch=token.epoch;return out;
        }catch(...){close();throw;}
    }
};
std::shared_ptr<Clip> morph(State& s,const Config& c,uint64_t epoch,std::string& error,ResidentWorker& resident){
    GenerationStatus generation(s,epoch,false);
    if(!generation.token.current())return {};
    // Existing PCM reader bounds size and validates RIFF before spawning.
    auto a=read_wav(c.morph_a,error);if(!a)return {};
    const bool blend=c.morph_effect=="blend";
    std::shared_ptr<Clip> b;if(blend){b=read_wav(c.morph_b,error);if(!b)return {};}
    if(a->sample_rate!=rate||a->source_channels!=2||(blend&&(b->sample_rate!=rate||b->source_channels!=2||a->frames!=b->frames))||
       a->frames<8192||a->frames>12*rate){error="Use stereo 44.1 kHz WAVs, 0.19–12 seconds; blend requires equal lengths";return {};}
    std::unique_lock<std::timed_mutex> gate(inference_gate,std::defer_lock);
    while(generation.token.current()&&!gate.try_lock_for(50ms)){}
    if(!generation.token.current()||!gate.owns_lock())return {};
    auto helper=std::filesystem::path(obs_get_module_binary_path(obs_current_module())).parent_path()/"sa3-runtime/sound-sa3-morph";
    const auto model=std::filesystem::path(c.models)/"stable-audio-3-small-music-same-s-v1.0-F16.gguf";
    if(!blend&&resident.start(helper.string(),model.string(),c.device=="cpu"?"cpu":"auto")){
        {std::lock_guard lock(s.mutex);if(!s.current(epoch)){resident.close();return {};}
            s.stage="morphing";s.message="Resident SAME-S processing";}
        auto clip=resident.request(*a,c,generation.token);
        generation.succeeded=true;
        blog(LOG_INFO,"[Sound/Morph] Resident SAME-S texture completed (%zu frames)",clip->frames);
        return clip;
    }

    std::array<char,32> pattern{};std::strcpy(pattern.data(),"/tmp/sound-sa3-morph.XXXXXX");
    const auto* dir=mkdtemp(pattern.data());
    if(!dir){error="Cannot create private morph workspace";return {};}
    struct Cleanup{std::filesystem::path dir;~Cleanup(){std::error_code ec;std::filesystem::remove_all(dir,ec);}} cleanup{dir};
    const auto output=cleanup.dir/"result.wav",log=cleanup.dir/"worker.log";
    std::vector<std::string> args={helper.string(),model.string(),c.morph_a,blend?c.morph_b:"",
        std::to_string(c.morph_mix),output.string(),c.device=="cpu"?"cpu":"auto",
        blend?(c.morph_ramp?"ramp":"fixed"):c.morph_effect};
    if(!blend){args.push_back(std::to_string(c.morph_position));args.push_back(std::to_string(c.morph_radius));args.push_back(std::to_string(c.morph_grain));}
    std::vector<char*> argv;for(auto& arg:args)argv.push_back(arg.data());argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions)){error="Cannot initialize morph worker";return {};}
    struct Actions{posix_spawn_file_actions_t* p;~Actions(){posix_spawn_file_actions_destroy(p);}} guard{&actions};
    if(posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0)||
       posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,log.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600)||
       posix_spawn_file_actions_adddup2(&actions,STDOUT_FILENO,STDERR_FILENO)){
        error="Cannot redirect morph worker";return {};
    }
    {std::lock_guard lock(s.mutex);if(!s.current(epoch))return {};s.stage="morphing";s.message="Encoding / mixing / decoding";}
    pid_t pid=0;
    const int spawned=posix_spawn(&pid,helper.c_str(),&actions,nullptr,argv.data(),environ);
    if(spawned){error="Cannot start native SAME-S worker";return {};}
    int status=0;
    const auto deadline=std::chrono::steady_clock::now()+45s;
    while(true){
        auto r=waitpid(pid,&status,WNOHANG);
        if(r==pid)break;
        if(r<0&&errno!=EINTR){error="Cannot wait for morph worker";return {};}
        if(!generation.token.current()||std::chrono::steady_clock::now()>=deadline){
            // This PID is our unreaped child, not an arbitrary user process.
            kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}
            error="Morph cancelled or timed out";return {};
        }
        std::unique_lock lock(s.mutex);s.cv.wait_for(lock,20ms,[&]{return !s.current(epoch);});
    }
    if(!s.current(epoch))return {};
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){error="Native morph failed; check equal stereo 44.1 kHz WAVs and verified SAME-S model";return {};}
    auto clip=read_wav(output.string(),error);
    if(!clip||clip->sample_rate!=rate||clip->frames!=a->frames){error="Invalid native morph output";return {};}
    generation.succeeded=true;
    blog(LOG_INFO,"[Sound/Morph] Native SAME-S morph completed (%zu frames)",clip->frames);
    return clip;
}
#endif
void worker(State* s){
#ifdef SOUND_WITH_SAME_MORPH
    ResidentWorker resident(*s);
#endif
    while(true){
        Config c;Job job;uint64_t e;std::shared_ptr<const Clip> clip;
        {std::unique_lock lock(s->mutex);
#ifdef SOUND_WITH_SAME_MORPH
         if(resident.active()){
            s->cv.wait_for(lock,8s,[&]{return s->closing||s->pending!=Job::None||!s->current(resident.epoch);});
            if(s->closing)return;
            if(s->pending==Job::None){lock.unlock();resident.close();continue;}
         }else
#endif
         s->cv.wait(lock,[&]{return s->closing||s->pending!=Job::None;});
         if(s->closing)return;
         job=s->pending;s->pending=Job::None;e=s->epoch;c=s->config;clip=s->saved;}
#ifdef SOUND_WITH_SAME_MORPH
        if(resident.active()&&(job!=Job::Morph||c.morph_effect=="blend"||e!=resident.epoch+1))resident.close();
#endif
        if(!s->current(e))continue;
        SessionEnd end{*s,e};
        try{
            if(job==Job::Continuous){continuous(*s,c,e);continue;}
            if(job==Job::Generate||job==Job::Transform||job==Job::Extend||job==Job::Morph){
                {std::lock_guard lock(s->mutex);if(!s->current(e))continue;++s->started;s->progress=0;s->message="Generating";}
                std::string error;
#ifdef SOUND_WITH_SAME_MORPH
                if(job==Job::Morph)clip=morph(*s,c,e,error,resident);
                else
#endif
                if(job==Job::Transform||job==Job::Extend){
                    if(!c.input_source.empty())clip=capture_input(*s,c,e,error);
                    else if(!c.input_file.empty())clip=read_wav(c.input_file,error);
                    if(clip)clip=generate(*s,c,e,error,clip.get(),job==Job::Extend);
                    else if(error.empty())error="Choose a WAV or generate a clip first";
                }else clip=generate(*s,c,e,error);
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
                else{++s->failed;++s->underruns;s->message="Playback deadline missed";}
            }
        }catch(const std::exception& ex){
            std::lock_guard lock(s->mutex);if(s->current(e)){++s->failed;s->message=ex.what();}
        }catch(...){
            std::lock_guard lock(s->mutex);if(s->current(e)){++s->failed;s->message="Unexpected diffusion error";}
        }
    }
}
void invalidate(State& s){
    ++s.epoch;s.pending=Job::None;s.progress=0;
    s.continuous_session=false;s.finish_requested=false;s.generating=false;
    s.buffer_frames=s.next_frames=0;s.stage="idle";
    s.adaptive_session=s.prefilling=false;s.queued_clips=s.target_clips=0;s.reserve_frames=0;s.cv.notify_all();
}
void update(void* p,obs_data_t* d){
    auto& s=*static_cast<State*>(p);Config c;
    c.models=obs_data_get_string(d,"models_dir");c.prompt=obs_data_get_string(d,"prompt");
    c.negative=obs_data_get_string(d,"negative_prompt");c.device=obs_data_get_string(d,"device");
    c.variant=std::string(obs_data_get_string(d,"variant"))=="small-sfx"?"small-sfx":"small-music";
    c.morph_a=obs_data_get_string(d,"morph_a");c.morph_b=obs_data_get_string(d,"morph_b");
    auto mix=obs_data_get_double(d,"morph_mix");c.morph_mix=std::isfinite(mix)?float(std::clamp(mix,0.,1.)):.5f;
    c.morph_ramp=obs_data_get_bool(d,"morph_ramp");
    const std::string effect=obs_data_get_string(d,"morph_effect");
    if(effect=="freeze"||effect=="smear"||effect=="grain")c.morph_effect=effect;
    auto position=obs_data_get_double(d,"morph_position");c.morph_position=std::isfinite(position)?float(std::clamp(position,0.,1.)):.5f;
    c.morph_radius=int(std::clamp<int64_t>(obs_data_get_int(d,"morph_radius"),1,16));
    c.morph_grain=int(std::clamp<int64_t>(obs_data_get_int(d,"morph_grain"),1,16));
    c.input_file=obs_data_get_string(d,"input_file");c.input_source=obs_data_get_string(d,"input_source");
    c.alternate_prompt=obs_data_get_string(d,"alternate_prompt");
    c.lora_a=obs_data_get_string(d,"lora_a");c.lora_b=obs_data_get_string(d,"lora_b");
    auto strength=[&](const char* key){double x=obs_data_get_double(d,key);return std::isfinite(x)?float(std::clamp(x,0.,2.)):1.f;};
    c.lora_a_strength=strength("lora_a_strength");c.lora_b_strength=strength("lora_b_strength");
    c.automate_b=obs_data_get_bool(d,"automate_b");
    c.sound_channel=obs_data_get_string(d,"sound_channel");c.sight_channel=obs_data_get_string(d,"sight_channel");
    auto capture=obs_data_get_double(d,"capture_seconds");c.capture_seconds=std::isfinite(capture)?std::clamp(capture,1.,30.):6.;
    c.capture_muted=obs_data_get_bool(d,"capture_muted");c.adaptive_buffer=obs_data_get_bool(d,"adaptive_buffer");
    auto noise=obs_data_get_double(d,"transform_noise");c.transform_noise=std::isfinite(noise)?float(std::clamp(noise,0.,1.)):.5f;
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
    if(job!=Job::Replay){auto* d=obs_source_get_settings(s.source);update(&s,d);obs_data_release(d);}
    std::lock_guard lock(s.mutex);
    if(!s.active||!s.enabled||s.closing){s.message="Activate the source first";return false;}
    if(job==Job::Replay&&!s.saved){s.message="Generate a clip first";return false;}
    invalidate(s);s.pending=job;s.message="Queued";
    s.continuous_session=job==Job::Continuous;
    s.adaptive_session=s.continuous_session&&s.config.adaptive_buffer;s.prefill_frames=0;
    s.session_frames=s.generation_ms=s.underruns=0;s.cv.notify_all();return false;
}
std::string status_description(void* p){
    std::string status="Generate while active. Q4_K_M / F16; local only. Continuous music uses 6–56 s segments. Selected source, WAV input or last generated clip for transform/extend (60 s result limit).";
    if(p){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);status+="\nStatus: "+s.message+" ("+std::to_string(s.progress.load())+"%)";
        status+="\nStage: "+std::string(s.stage)+" | Last generation: "+std::to_string(s.generation_ms)+" ms";
        status+="\nCurrent buffer: "+std::to_string(s.buffer_frames*1000/rate)+" ms | Next clip: "+std::to_string(s.next_frames*1000/rate)+" ms";
        status+="\nPlayed: "+std::to_string(s.session_frames/rate)+" s | Underruns: "+std::to_string(s.underruns);
        if(s.resident_active)status+="\nSAME-S warm | Requests: "+std::to_string(s.resident_requests.load())+" | Graph pairs: "+std::to_string(s.resident_graph_builds.load());
        if(s.adaptive_session)status+="\nReserve: "+std::to_string(s.reserve_frames*1000/rate)+" ms | "+std::to_string(s.queued_clips)+"/"+std::to_string(s.target_clips)+" clips"+(s.prefilling?" | Prefilling":"");}
    return status;
}
bool refresh(obs_properties_t* props,obs_property_t*,void* p){
    if(auto* note=obs_properties_get(props,"note")){
        const auto description=status_description(p);
        obs_property_set_description(note,description.c_str());
    }
    return true;
}
bool make(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Generate);}
bool start_continuous(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Continuous);}
bool transform(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Transform);}
bool extend(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Extend);}
#ifdef SOUND_WITH_SAME_MORPH
bool morph_button(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Morph);}
#endif
bool replay(obs_properties_t*,obs_property_t*,void* p){return queue(p,Job::Replay);}
bool finish_segment(obs_properties_t*,obs_property_t*,void* p){
    if(p){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);
        if(s.continuous_session){s.finish_requested=true;s.message="Finishing current segment";s.cv.notify_all();}
    }return false;
}
bool stop(obs_properties_t*,obs_property_t*,void* p){
    if(p){auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);invalidate(s);s.message="Stopped";}return false;
}
void defaults(obs_data_t* d){
    obs_data_set_default_double(d,"capture_seconds",6);
    obs_data_set_default_double(d,"morph_mix",.5);
    obs_data_set_default_string(d,"morph_effect","blend");
    obs_data_set_default_double(d,"morph_position",.5);
    obs_data_set_default_int(d,"morph_radius",2);obs_data_set_default_int(d,"morph_grain",2);
    obs_data_set_default_bool(d,"adaptive_buffer",false);
    obs_data_set_default_double(d,"lora_a_strength",1);obs_data_set_default_double(d,"lora_b_strength",1);
    obs_data_set_default_string(d,"variant","small-music");obs_data_set_default_double(d,"transform_noise",.5);
    obs_data_set_default_string(d,"models_dir","");obs_data_set_default_string(d,"device","auto");
    obs_data_set_default_string(d,"prompt","warm instrumental bedroom pop, soft drums, toy keys");
    obs_data_set_default_double(d,"duration",12);obs_data_set_default_int(d,"steps",8);obs_data_set_default_int(d,"seed",-1);
}
obs_properties_t* props(void* p){
    auto* d=obs_properties_create();
    obs_properties_add_path(d,"models_dir","Stable Audio 3 GGUF model directory",OBS_PATH_DIRECTORY,nullptr,nullptr);
    auto* variant=obs_properties_add_list(d,"variant","Model family",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(variant,"Small Music","small-music");obs_property_list_add_string(variant,"Small SFX","small-sfx");
    auto* device=obs_properties_add_list(d,"device","Compute device",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(device,"Auto (GPU preferred)","auto");obs_property_list_add_string(device,"CPU","cpu");
    obs_properties_add_text(d,"prompt","Prompt",OBS_TEXT_MULTILINE);
    obs_properties_add_text(d,"negative_prompt","Negative prompt",OBS_TEXT_DEFAULT);
    obs_properties_add_float_slider(d,"duration","Duration (seconds)",1,60,1);
    obs_properties_add_int_slider(d,"steps","Diffusion steps",1,32,1);
    obs_properties_add_int(d,"seed","Seed (-1 = random)",-1,2147483647,1);
    obs_properties_add_button2(d,"generate","Generate and play",make,p);
    obs_properties_add_bool(d,"adaptive_buffer","Adaptive playback reserve (longer startup)");
    obs_properties_add_button2(d,"continuous","Start continuous music",start_continuous,p);
    obs_properties_add_button2(d,"finish_segment","Finish current segment",finish_segment,p);
    obs_properties_add_button2(d,"replay","Replay last clip",replay,p);
    auto* input=obs_properties_add_list(d,"input_source","Capture audio source (optional)",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(input,"None (WAV / last clip)","");
    obs_enum_sources([](void* p,obs_source_t* source){
        if(capture_source(source)&&!obs_source_removed(source))
            obs_property_list_add_string(static_cast<obs_property_t*>(p),obs_source_get_name(source),obs_source_get_uuid(source));
        return true;
    },input);
    obs_properties_add_float_slider(d,"capture_seconds","Capture duration (seconds)",1,30,1);
    obs_properties_add_bool(d,"capture_muted","Capture muted source");
    obs_properties_add_path(d,"input_file","Input WAV (optional, 44.1 / 48 kHz PCM16)",OBS_PATH_FILE,"WAV (*.wav)",nullptr);
    obs_properties_add_float_slider(d,"transform_noise","Transform strength",0,1,.05);
    obs_properties_add_button2(d,"transform","Transform input / last clip",transform,p);
    obs_properties_add_button2(d,"extend","Extend input / last clip",extend,p);
#ifdef SOUND_WITH_SAME_MORPH
    auto* morph_props=obs_properties_create();
    obs_properties_add_path(morph_props,"morph_a","Audio A (stereo 44.1 kHz WAV)",OBS_PATH_FILE,"WAV (*.wav)",nullptr);
    obs_properties_add_path(morph_props,"morph_b","Audio B (same length)",OBS_PATH_FILE,"WAV (*.wav)",nullptr);
    auto* effect=obs_properties_add_list(morph_props,"morph_effect","Operation",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(effect,"A/B morph","blend");obs_property_list_add_string(effect,"Freeze","freeze");
    obs_property_list_add_string(effect,"Temporal smear","smear");obs_property_list_add_string(effect,"Latent grain repeat","grain");
    obs_properties_add_float_slider(morph_props,"morph_mix","Blend / effect amount",0,1,.01);
    obs_properties_add_float_slider(morph_props,"morph_position","Freeze / grain position",0,1,.01);
    obs_properties_add_int_slider(morph_props,"morph_radius","Smear radius (latent frames)",1,16,1);
    obs_properties_add_int_slider(morph_props,"morph_grain","Grain length (latent frames)",1,16,1);
    obs_properties_add_bool(morph_props,"morph_ramp","Ramp from A toward selected blend");
    obs_properties_add_button2(morph_props,"morph","Process and play",morph_button,p);
    obs_properties_add_group(d,"latent_morph","Neural latent morph / texture (SAME-S)",OBS_GROUP_NORMAL,morph_props);
#endif
    obs_properties_add_button2(d,"stop","Stop / cancel",stop,p);
    auto* advanced=obs_properties_create();
    obs_properties_add_text(advanced,"alternate_prompt","Fade toward prompt (optional)",OBS_TEXT_MULTILINE);
    obs_properties_add_path(advanced,"lora_a","LoRA A (compatible GGUF)",OBS_PATH_FILE,"GGUF (*.gguf)",nullptr);
    obs_properties_add_float_slider(advanced,"lora_a_strength","LoRA A strength",0,2,.05);
    obs_properties_add_path(advanced,"lora_b","LoRA B (compatible GGUF)",OBS_PATH_FILE,"GGUF (*.gguf)",nullptr);
    obs_properties_add_float_slider(advanced,"lora_b_strength","LoRA B strength",0,2,.05);
    obs_properties_add_bool(advanced,"automate_b","Fade in LoRA B across generated segment");
    auto* sound=obs_properties_add_list(advanced,"sound_channel","Sound context (optional)",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(sound,"None","");
    for(const auto& e:sound_signals::discover())
        if(e.kind==sound_signals::Kind::Transcript||e.kind==sound_signals::Kind::Classification)
            obs_property_list_add_string(sound,e.label.c_str(),e.channel.c_str());
    auto* sight=obs_properties_add_list(advanced,"sight_channel","Sight motion (optional)",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(sight,"None","");
    obs_enum_sources([](void* p,obs_source_t* parent){
        obs_source_enum_filters(parent,[](obs_source_t* parent,obs_source_t* filter,void* p){
            if(sight_filter(filter)){
                auto key=sound_signals::channel(parent,filter);
                auto label=std::string(obs_source_get_name(parent))+" / "+obs_source_get_name(filter);
                if(!key.empty())obs_property_list_add_string(static_cast<obs_property_t*>(p),label.c_str(),key.c_str());
            }
        },p);return true;
    },sight);
    obs_properties_add_group(d,"conditioning","Prompt automation and reactive conditioning",OBS_GROUP_NORMAL,advanced);
    obs_properties_add_button2(d,"refresh_status","Refresh status",refresh,p);
    const auto status=status_description(p);
    obs_properties_add_text(d,"note",status.c_str(),OBS_TEXT_INFO);
    return d;
}
void status(void* p,calldata_t* d){
    auto& s=*static_cast<State*>(p);
    calldata_set_int(d,"started",s.started);calldata_set_int(d,"completed",s.completed);
    calldata_set_int(d,"played",s.played);calldata_set_int(d,"failed",s.failed);calldata_set_int(d,"progress",s.progress);
    std::lock_guard lock(s.mutex);calldata_set_string(d,"message",s.message.c_str());
}
void status_v2(void* p,calldata_t* d){
    status(p,d);auto& s=*static_cast<State*>(p);
    calldata_set_int(d,"captured",s.captured);calldata_set_int(d,"automated",s.automated);
    calldata_set_int(d,"sound_cues",s.sound_cues);calldata_set_int(d,"sight_cues",s.sight_cues);
}
void status_v3(void* p,calldata_t* d){
    status_v2(p,d);
    auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);
    calldata_set_bool(d,"continuous",s.continuous_session);calldata_set_bool(d,"finishing",s.finish_requested);
    calldata_set_bool(d,"generating",s.generating);calldata_set_bool(d,"next_ready",s.next_frames!=0);
    calldata_set_int(d,"generation_ms",s.generation_ms);calldata_set_int(d,"buffer_ms",s.buffer_frames*1000/rate);
    calldata_set_int(d,"played_ms",s.session_frames/rate*1000+(s.session_frames%rate)*1000/rate);
    calldata_set_int(d,"underruns",s.underruns);calldata_set_int(d,"next_ms",s.next_frames*1000/rate);
    calldata_set_string(d,"stage",s.stage);
}
void buffer_status(void* p,calldata_t* d){
    auto& s=*static_cast<State*>(p);std::lock_guard lock(s.mutex);
    calldata_set_bool(d,"adaptive",s.adaptive_session);calldata_set_bool(d,"prefilling",s.prefilling);
    calldata_set_int(d,"queued_clips",s.queued_clips);calldata_set_int(d,"target_clips",s.target_clips);
    calldata_set_int(d,"reserve_ms",s.reserve_frames*1000/rate);
    calldata_set_int(d,"prefill_ms",s.prefill_frames*1000/rate);
}
#ifdef SOUND_WITH_SAME_MORPH
void resident_status(void* p,calldata_t* d){
    auto& s=*static_cast<State*>(p);
    calldata_set_int(d,"starts",s.resident_starts);calldata_set_int(d,"requests",s.resident_requests);
    calldata_set_int(d,"graph_builds",s.resident_graph_builds);calldata_set_int(d,"active",s.resident_active?1:0);
}
#endif
void* create(obs_data_t* d,obs_source_t* source){
    try{
        auto s=std::make_unique<State>(source);update(s.get(),d);
        s->worker=std::thread(worker,s.get());
        signal_handler_connect(obs_source_get_signal_handler(source),"enable",enabled,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),
          "void sound_diffusion_status_v1(out int started, out int completed, out int played, out int failed, out int progress, out string message)",status,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),
          "void sound_diffusion_status_v2(out int started, out int completed, out int played, out int failed, out int progress, out string message, out int captured, out int automated, out int sound_cues, out int sight_cues)",status_v2,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),
          "void sound_diffusion_status_v3(out int started, out int completed, out int played, out int failed, out int progress, out string message, out int captured, out int automated, out int sound_cues, out int sight_cues, out bool continuous, out bool finishing, out bool generating, out bool next_ready, out int generation_ms, out int buffer_ms, out int played_ms, out int underruns, out int next_ms, out string stage)",status_v3,s.get());
        proc_handler_add(obs_source_get_proc_handler(source),
          "void sound_diffusion_buffer_v1(out bool adaptive, out bool prefilling, out int queued_clips, out int target_clips, out int reserve_ms, out int prefill_ms)",buffer_status,s.get());
#ifdef SOUND_WITH_SAME_MORPH
        proc_handler_add(obs_source_get_proc_handler(source),
          "void sound_same_resident_v1(out int starts, out int requests, out int graph_builds, out int active)",resident_status,s.get());
#endif
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
