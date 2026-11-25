#include <obs-module.h>
#include <util/platform.h>
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <unistd.h>
#include "framework/tts_wav.hpp"
#include "framework/tiktok_native.hpp"
#include "framework/tts_voice_catalog.hpp"
#include "framework/tts_text.hpp"
#include "interop/sound_signal_client.hpp"

namespace {
using namespace std::chrono_literals;
enum class Engine { Kokoro, KokoroNative, Piper, Chatterbox, ChatterboxNative, TikTok, Edge, Invalid };
constexpr Engine backends[]={Engine::Kokoro,Engine::KokoroNative,Engine::Piper,Engine::Chatterbox,Engine::ChatterboxNative,Engine::TikTok,Engine::Edge};
const char* model(Engine e){
    switch(e){case Engine::Kokoro:return "kokoro";case Engine::KokoroNative:return "kokoro_native";case Engine::Piper:return "piper";
        case Engine::Chatterbox:return "chatterbox";case Engine::ChatterboxNative:return "chatterbox_native";case Engine::TikTok:return "tiktok";case Engine::Edge:return "edge";default:return "invalid";}
}
const char* default_voice(Engine e){
    switch(e){case Engine::Kokoro:case Engine::KokoroNative:return "af_heart";case Engine::Piper:return "en_US-amy-low";
        case Engine::Chatterbox:case Engine::ChatterboxNative:return "default";case Engine::TikTok:return "en_us_001";case Engine::Edge:return "en-US-AriaNeural";default:return "";}
}
Engine parse_engine(const char* id){
    for(auto e:backends)if(!strcmp(id,model(e)))return e;
    return Engine::Invalid;
}
const char* voice_key(Engine e){
    switch(e){case Engine::Kokoro:return "voice_kokoro";case Engine::KokoroNative:return "voice_kokoro_native";case Engine::Piper:return "voice_piper";
        case Engine::Chatterbox:return "voice_chatterbox";case Engine::ChatterboxNative:return "voice_chatterbox_native";case Engine::TikTok:return "voice_tiktok";case Engine::Edge:return "voice_edge";default:return "voice_invalid";}
}
const char* engine_label(Engine e){
    switch(e){case Engine::Kokoro:return "Kokoro (local ONNX)";case Engine::KokoroNative:return "Kokoro (native GGUF CUDA)";case Engine::Piper:return "Piper (local ONNX)";
        case Engine::Chatterbox:return "Chatterbox Nano (local CUDA)";case Engine::ChatterboxNative:return "Chatterbox Turbo (native GGUF CPU)";case Engine::TikTok:return "TikTok (cloud)";case Engine::Edge:return "Edge TTS (cloud)";default:return "Unavailable";}
}
bool local_model(Engine e){return e==Engine::Kokoro||e==Engine::KokoroNative||e==Engine::Piper||e==Engine::Chatterbox||e==Engine::ChatterboxNative;}
std::string local_socket(Engine e){
    const auto* dir=std::getenv("XDG_RUNTIME_DIR");
    if(!dir||!*dir)return {};
    return std::string(dir)+(e==Engine::Chatterbox?"/lll-sound-chatterbox/speech.sock":
        e==Engine::Edge?"/lll-sound-edge/speech.sock":"/lll-sound-tts/speech.sock");
}
int retention_ttl(obs_data_t* settings){
    const auto mode=obs_data_get_int(settings,"retention_mode");
    return mode==2?-1:mode==1?int(std::clamp<int64_t>(obs_data_get_int(settings,"retention_ttl"),1,3600)):0;
}
enum class OptionKind { Float, Int, Bool, Language, Reference, Note };
struct Option {
    Engine engine;
    const char* key;
    const char* param;
    const char* label;
    OptionKind kind;
    double initial=0,low=0,high=0,step=0;
    const char* detail=nullptr;
};
constexpr Option options[]={
    {Engine::ChatterboxNative,"chatterbox_native_temperature","temperature","Temperature",OptionKind::Float,.8,.1,1.5,.05},
    {Engine::ChatterboxNative,"chatterbox_native_top_k","top_k","Top-K (0 = disabled)",OptionKind::Int,1000,0,6563,1},
    {Engine::ChatterboxNative,"chatterbox_native_top_p","top_p","Top-P",OptionKind::Float,.95,.05,1,.01},
    {Engine::ChatterboxNative,"chatterbox_native_repetition_penalty","repetition_penalty","Repetition penalty",OptionKind::Float,1.2,.5,2,.05},
    {Engine::ChatterboxNative,"chatterbox_native_note",nullptr,"CPU-only native reader; slower than real time on the qualified machine. Built-in voice only. Use Nano for reference cloning. Supports [laugh] and [sigh].",OptionKind::Note},
    {Engine::KokoroNative,"kokoro_native_speed","speed","Speed",OptionKind::Float,1,.5,2,.05},
    {Engine::KokoroNative,"kokoro_native_note",nullptr,"Requires the configured native reader helper. Voice-matched pronunciation; phoneme input, batch trim and pause controls require ONNX.",OptionKind::Note},
    {Engine::Kokoro,"kokoro_speed","speed","Speed",OptionKind::Float,1,.5,2,.05},
    {Engine::Kokoro,"kokoro_language","language","Pronunciation",OptionKind::Language},
    {Engine::Kokoro,"kokoro_is_phonemes","is_phonemes","Input is phonemes",OptionKind::Bool},
    {Engine::Kokoro,"kokoro_trim","trim","Trim batch silence",OptionKind::Bool,1,0,0,0,"Trim internal phoneme-batch silence."},
    {Engine::Kokoro,"kokoro_sentence_pause","sentence_pause","Sentence pause (s)",OptionKind::Float,.25,0,2,.05,"Applied at internal batch joins by the current audio-only model."},
    {Engine::Kokoro,"kokoro_clause_pause","clause_pause","Clause pause (s)",OptionKind::Float,.1,0,2,.05,"Applied at internal batch joins by the current audio-only model."},
    {Engine::Piper,"piper_length_scale","length_scale","Phoneme length scale",OptionKind::Float,1,.5,2,.05,"Lower is faster; higher is slower."},
    {Engine::Piper,"piper_noise_scale","noise_scale","Generator noise",OptionKind::Float,.667,0,2,.001},
    {Engine::Piper,"piper_noise_w_scale","noise_w_scale","Phoneme width noise",OptionKind::Float,.8,0,2,.01},
    {Engine::Piper,"piper_normalize_audio","normalize_audio","Normalize speech",OptionKind::Bool,1},
    {Engine::Piper,"piper_volume","volume","Synthesis volume",OptionKind::Float,1,.05,2,.05},
    {Engine::Chatterbox,"chatterbox_temperature","temperature","Temperature",OptionKind::Float,.8,.1,1.5,.05},
    {Engine::Chatterbox,"chatterbox_top_k","top_k","Top-K (0 = disabled)",OptionKind::Int,1000,0,6563,1},
    {Engine::Chatterbox,"chatterbox_top_p","top_p","Top-P",OptionKind::Float,.95,.05,1,.01},
    {Engine::Chatterbox,"chatterbox_repetition_penalty","repetition_penalty","Repetition penalty",OptionKind::Float,1.2,.5,2,.05},
    {Engine::Chatterbox,"chatterbox_audio_prompt_path","audio_prompt_path","Reference audio (optional)",OptionKind::Reference,0,0,0,0,"Local WAV/FLAC, longer than 5 seconds, up to 60 seconds and 32 MiB. Use only recordings you have permission to use. Blank restores the built-in voice."},
    {Engine::Chatterbox,"chatterbox_norm_loudness","norm_loudness","Normalize reference loudness",OptionKind::Bool,1,0,0,0,"Only applies to reference audio."},
    {Engine::Edge,"edge_rate","rate","Speech rate (%)",OptionKind::Int,0,-50,100,1},
    {Engine::Edge,"edge_pitch","pitch","Pitch (Hz)",OptionKind::Int,0,-50,50,1},
    {Engine::Edge,"edge_volume","volume","Synthesis volume (%)",OptionKind::Int,0,-90,100,1},
    {Engine::TikTok,"tiktok_controls_note",nullptr,"TikTok supports voice selection only; use OBS audio filters for post-processing.",OptionKind::Note},
};
void option_defaults(obs_data_t* d){
    for(const auto& o:options){
        switch(o.kind){
        case OptionKind::Float:obs_data_set_default_double(d,o.key,o.initial);break;
        case OptionKind::Int:obs_data_set_default_int(d,o.key,int64_t(o.initial));break;
        case OptionKind::Bool:obs_data_set_default_bool(d,o.key,o.initial!=0);break;
        case OptionKind::Language:obs_data_set_default_string(d,o.key,"auto");break;
        case OptionKind::Reference:obs_data_set_default_string(d,o.key,"");break;
        default:break;
        }
    }
}
std::string selected_options(obs_data_t* d,Engine engine){
    auto* out=obs_data_create();
    for(const auto& o:options){
        if(o.engine!=engine||!o.param)continue;
        switch(o.kind){
        case OptionKind::Float:{
            const auto v=obs_data_get_double(d,o.key);
            obs_data_set_double(out,o.param,std::isfinite(v)?std::clamp(v,o.low,o.high):o.initial);break;
        }
        case OptionKind::Int:obs_data_set_int(out,o.param,std::clamp<int64_t>(obs_data_get_int(d,o.key),int64_t(o.low),int64_t(o.high)));break;
        case OptionKind::Bool:obs_data_set_bool(out,o.param,obs_data_get_bool(d,o.key));break;
        case OptionKind::Language:case OptionKind::Reference:obs_data_set_string(out,o.param,obs_data_get_string(d,o.key));break;
        default:break;
        }
    }
    std::string json=obs_data_get_json(out);obs_data_release(out);return json;
}
void option_visibility(obs_properties_t* p,Engine engine,obs_data_t* d=nullptr){
    for(const auto& o:options){
        if(auto* field=obs_properties_get(p,o.key)){
            bool show=o.engine==engine;
            if(show&&d){
                if(o.kind==OptionKind::Language)show=!obs_data_get_bool(d,"kokoro_is_phonemes");
                else if(!strcmp(o.key,"chatterbox_norm_loudness"))show=*obs_data_get_string(d,"chatterbox_audio_prompt_path")!=0;
            }
            obs_property_set_visible(field,show);
        }
    }
}
bool option_changed(obs_properties_t* p,obs_property_t*,obs_data_t* d){
    option_visibility(p,parse_engine(obs_data_get_string(d,"backend")),d);return true;
}
void add_options(obs_properties_t* p){
    for(const auto& o:options){
        obs_property_t* field=nullptr;
        switch(o.kind){
        case OptionKind::Float:field=obs_properties_add_float_slider(p,o.key,o.label,o.low,o.high,o.step);break;
        case OptionKind::Int:field=obs_properties_add_int_slider(p,o.key,o.label,int(o.low),int(o.high),int(o.step));break;
        case OptionKind::Bool:field=obs_properties_add_bool(p,o.key,o.label);break;
        case OptionKind::Language:
            field=obs_properties_add_list(p,o.key,o.label,OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
            obs_property_list_add_string(field,"Match voice","auto");
            obs_property_list_add_string(field,"English (US)","en-us");
            obs_property_list_add_string(field,"English (UK)","en-gb");break;
        case OptionKind::Reference:field=obs_properties_add_path(p,o.key,o.label,OBS_PATH_FILE,"Audio (*.wav *.flac)",nullptr);break;
        case OptionKind::Note:field=obs_properties_add_text(p,o.key,o.label,OBS_TEXT_INFO);break;
        }
        if(o.detail)obs_property_set_long_description(field,o.detail);
        if(!strcmp(o.key,"kokoro_is_phonemes")||o.kind==OptionKind::Reference)
            obs_property_set_modified_callback(field,option_changed);
    }
}
struct Config {
    std::string voice,uuid,text;
    bool monitor=false;
    Engine selected=Engine::Invalid;
    int ttl=0;
    std::string synthesis_options;
    std::string stt_source;
    bool stt=false,plugin_input=false,stt_allow_cloud=false;
};
struct State {
    obs_source_t* context;
    std::mutex mutex;
    std::condition_variable cv;
    Config config;
    std::string pending,observed,spoken;
    std::deque<std::string> relay_queue;
    uint64_t relay_sequence=0;
    bool relay_bound=false;
    std::chrono::steady_clock::time_point changed{},next_poll{};
    std::atomic<bool> stopping{false},active{false},enabled{true};
    std::atomic<uint64_t> generation{0},completed{0},failed{0};
    std::thread worker,lease_worker;
    const std::string lease_id;
    State(obs_source_t* c):context(c),
        lease_id(std::to_string(getpid())+"-"+std::to_string(os_gettime_ns())+"-"+std::to_string(reinterpret_cast<uintptr_t>(this))){}
    bool current(uint64_t g) const {
        return !stopping.load()&&active.load()&&enabled.load()&&generation.load()==g;
    }
};
bool known_voice(Engine e,const std::string& voice){
    if(e==Engine::Kokoro||e==Engine::KokoroNative){for(auto v:stts::KokoroVoices)if(voice==v.id)return true;return false;}
    if(e==Engine::TikTok){for(auto v:stts::TikTokVoices)if(voice==v.id)return true;return false;}
    if(e==Engine::Edge){for(auto v:stts::EdgeVoices)if(voice==v.id)return true;return false;}
    return (e==Engine::Piper||e==Engine::Chatterbox||e==Engine::ChatterboxNative)&&voice==default_voice(e);
}
struct Transfer {
    State* state;
    uint64_t generation;
    std::vector<uint8_t> bytes;
    bool native_options=false;
    size_t max_bytes=9*1024*1024;
};
size_t receive(char* p,size_t size,size_t nmemb,void* opaque){
    auto& t=*static_cast<Transfer*>(opaque);
    const size_t max=t.max_bytes;
    if(size&&nmemb>max/size)return 0;
    size_t n=size*nmemb;
    if(n>max-t.bytes.size()||!t.state->current(t.generation))return 0;
    t.bytes.insert(t.bytes.end(),p,p+n);return n;
}
size_t receive_header(char* p,size_t size,size_t nmemb,void* opaque){
    auto& t=*static_cast<Transfer*>(opaque);
    const size_t n=size*nmemb;
    const std::string line(p,n);
    if(line.rfind("HTTP/",0)==0)t.native_options=false;
    constexpr const char* key="x-sound-tts-options:";
    if(line.size()>=strlen(key)&&strncasecmp(line.c_str(),key,strlen(key))==0){
        auto value=line.substr(strlen(key));
        auto first=value.find_first_not_of(" \t\r\n");
        auto last=value.find_last_not_of(" \t\r\n");
        t.native_options=first!=std::string::npos&&value.substr(first,last-first+1)=="1";
    }
    return n;
}
int progress(void* p,curl_off_t,curl_off_t,curl_off_t,curl_off_t){
    auto& t=*static_cast<Transfer*>(p);
    return t.state->current(t.generation)?0:1;
}
bool synth(State& s,Engine engine,const std::string& voice,const std::string& text,uint64_t g,int ttl,const std::string& options_json,stts::TtsPcm& pcm){
    if(text.size()>stts::MaxChunkBytes||!stts::valid_speech_text(text)||!known_voice(engine,voice))return false;
    const bool tiktok=engine==Engine::TikTok;
    std::string socket;
    if(!tiktok){
        socket=local_socket(engine);
        if(socket.empty())return false;
    }else if(text.size()>stts::TikTokChunkBytes)return false;
    auto* args=obs_data_create();
    obs_data_set_string(args,"voice",voice.c_str());
    if(tiktok){
        obs_data_set_string(args,"text",text.c_str());
    }else{
        obs_data_set_string(args,"model",model(engine));
        obs_data_set_string(args,"input",text.c_str());
        obs_data_set_string(args,"response_format","wav");
        if(local_model(engine))obs_data_set_int(args,"retention_ttl",ttl);
        auto* opts=obs_data_create_from_json(options_json.c_str());
        if(!opts){obs_data_release(args);return false;}
        obs_data_set_obj(args,"options",opts);obs_data_release(opts);
    }
    std::string json=obs_data_get_json(args);
    obs_data_release(args);
    auto* curl=curl_easy_init();
    if(!curl)return false;
    auto* headers=curl_slist_append(nullptr,"Content-Type: application/json");
    Transfer transfer{&s,g,{}};
    curl_easy_setopt(curl,CURLOPT_URL,tiktok?stts::TikTokEndpoint:"http://localhost/v1/audio/speech");
    if(tiktok){
        transfer.max_bytes=stts::TikTokMaxResponse;
        curl_easy_setopt(curl,CURLOPT_USERAGENT,"LiveLatentLab-Sound/1.0");
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
        curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
    }
    if(!tiktok)curl_easy_setopt(curl,CURLOPT_UNIX_SOCKET_PATH,socket.c_str());
    curl_easy_setopt(curl,CURLOPT_PROXY,"");
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,tiktok?5000L:1500L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,tiktok?15000L:45000L);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json.c_str());
    curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,long(json.size()));
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&transfer);
    curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,receive_header);
    curl_easy_setopt(curl,CURLOPT_HEADERDATA,&transfer);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&transfer);
    const auto result=curl_easy_perform(curl);
    long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    curl_slist_free_all(headers);curl_easy_cleanup(curl);
    if(!tiktok&&result==CURLE_OK&&status==200&&!transfer.native_options){
        blog(LOG_WARNING,"[Sound/TTS] %s worker is older than the native options protocol; restart the service",model(engine));
        return false;
    }
    if(result!=CURLE_OK||status!=200||!s.current(g))return false;
    if(tiktok){
        if(!stts::decode_tiktok_response(std::string(transfer.bytes.begin(),transfer.bytes.end()),pcm)||!s.current(g))return false;
        blog(LOG_INFO,"[Sound/TTS] SOUND_TIKTOK_NATIVE_HTTP_PASS rate=%u frames=%zu",pcm.rate,pcm.samples.size());
        return true;
    }
    return stts::decode_tts_wav(transfer.bytes,pcm);
}
bool play(State& s,const stts::TtsPcm& pcm,uint64_t g){
    // Timestamp every packet from one sample clock; no render/video timing.
    const auto start=std::chrono::steady_clock::now();
    const auto clock=os_gettime_ns();
    const size_t block=pcm.rate/50;
    for(size_t i=0;i<pcm.samples.size();i+=block){
        if(!s.current(g))return false;
        const auto elapsed=std::chrono::nanoseconds(uint64_t(i)*1000000000ULL/pcm.rate);
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.cv.wait_until(lock,start+elapsed,[&]{return !s.current(g);});
        }
        if(!s.current(g))return false;
        if(std::chrono::steady_clock::now()>start+elapsed+250ms)return false;
        obs_source_audio audio{};
        audio.data[0]=reinterpret_cast<const uint8_t*>(pcm.samples.data()+i);
        audio.frames=uint32_t(std::min(block,pcm.samples.size()-i));
        audio.format=AUDIO_FORMAT_FLOAT_PLANAR;audio.speakers=SPEAKERS_MONO;
        audio.samples_per_sec=pcm.rate;audio.timestamp=clock+uint64_t(elapsed.count());
        obs_source_output_audio(s.context,&audio);
    }
    // Keep the last packet's duration before the next utterance.
    std::unique_lock<std::mutex> lock(s.mutex);
    s.cv.wait_until(lock,start+std::chrono::nanoseconds(uint64_t(pcm.samples.size())*1000000000ULL/pcm.rate),
                    [&]{return !s.current(g);});
    return s.current(g);
}
void run(State* s){
    while(!s->stopping.load()){
        std::string text,voice,options_json;uint64_t g;Engine engine;int ttl;
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            s->cv.wait(lock,[&]{return s->stopping.load()||(s->active.load()&&s->enabled.load()&&(!s->pending.empty()||!s->relay_queue.empty()));});
            if(s->stopping.load())break;
            if(!s->pending.empty()){text=std::move(s->pending);s->pending.clear();}
            else{text=std::move(s->relay_queue.front());s->relay_queue.pop_front();}
            voice=s->config.voice;engine=s->config.selected;ttl=s->config.ttl;options_json=s->config.synthesis_options;g=s->generation.load();
        }
        try {
            auto chunks=stts::split_speech(text,engine==Engine::TikTok?stts::TikTokChunkBytes:engine==Engine::ChatterboxNative?size_t(128):stts::MaxChunkBytes);
            bool ok=!chunks.empty();
            size_t played=0,total_frames=0;
            for(const auto& chunk:chunks){
                if(!s->current(g)){ok=false;break;}
                if(!stts::has_speech(chunk))continue;
                stts::TtsPcm pcm;
                if(!synth(*s,engine,voice,chunk,g,ttl,options_json,pcm)||!play(*s,pcm,g)){ok=false;break;}
                ++played;total_frames+=pcm.samples.size();
                blog(LOG_INFO,"[Sound/TTS] SOUND_TTS_CLOCKED_20261118_T721 backend=%s rate=%u frames=%zu",
                     model(engine),pcm.rate,pcm.samples.size());
            }
            if(ok&&played&&s->current(g)){
                ++s->completed;
                blog(LOG_INFO,"[Sound/TTS] SOUND_TTS_LONG_TEXT_20261118_L673 backend=%s bytes=%zu chunks=%zu frames=%zu",
                     model(engine),text.size(),played,total_frames);
            } else if(s->current(g)){
                ++s->failed;
                blog(LOG_WARNING,"[Sound/TTS] %s failed after %zu chunks; remaining text not spoken",model(engine),played);
            }
        }catch(...){if(s->current(g))++s->failed;}
    }
}
size_t discard_reply(char*,size_t size,size_t count,void*){return size*count;}
bool lease_request(State& s,Engine engine,bool acquire,int ttl){
    const auto socket=local_socket(engine);
    if(socket.empty())return false;
    auto* args=obs_data_create();
    obs_data_set_string(args,"model",model(engine));
    obs_data_set_string(args,"client",s.lease_id.c_str());
    obs_data_set_string(args,"action",acquire?"acquire":"release");
    obs_data_set_int(args,"ttl",ttl);
    std::string json=obs_data_get_json(args);obs_data_release(args);
    auto* curl=curl_easy_init();if(!curl)return false;
    auto* headers=curl_slist_append(nullptr,"Content-Type: application/json");
    curl_easy_setopt(curl,CURLOPT_URL,"http://localhost/v1/models/lease");
    curl_easy_setopt(curl,CURLOPT_UNIX_SOCKET_PATH,socket.c_str());
    curl_easy_setopt(curl,CURLOPT_PROXY,"");
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,400L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,1200L);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json.c_str());
    curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,long(json.size()));
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,discard_reply);
    const auto result=curl_easy_perform(curl);
    long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    curl_slist_free_all(headers);curl_easy_cleanup(curl);
    return result==CURLE_OK&&status==200;
}
Engine wanted_model(const State& s){
    return !s.stopping.load()&&s.active.load()&&s.enabled.load()&&local_model(s.config.selected)?s.config.selected:Engine::Invalid;
}
void maintain_leases(State* s){
    constexpr Engine local[]={Engine::Kokoro,Engine::KokoroNative,Engine::Piper,Engine::Chatterbox,Engine::ChatterboxNative};
    bool touched[sizeof(local)/sizeof(local[0])]={};
    while(true){
        Engine desired;int ttl;bool stopping;
        {
            std::lock_guard lock(s->mutex);
            desired=wanted_model(*s);ttl=s->config.ttl;stopping=s->stopping.load();
        }
        for(size_t i=0;i<sizeof(local)/sizeof(local[0]);++i){
            if(local[i]!=desired&&touched[i]&&lease_request(*s,local[i],false,0))touched[i]=false;
        }
        if(stopping)break; // failed release expires via the bounded server lease
        for(size_t i=0;i<sizeof(local)/sizeof(local[0]);++i)if(local[i]==desired){
            touched[i]=true; // even a timed-out request might have reached server
            lease_request(*s,local[i],true,ttl);
        }
        std::unique_lock lock(s->mutex);
        s->cv.wait_for(lock,2s,[&]{return s->stopping.load()||wanted_model(*s)!=desired||s->config.ttl!=ttl;});
    }
}
bool text_source(obs_source_t* source){
    const char* id=obs_source_get_unversioned_id(source);
    return id&&(!strcmp(id,"text_ft2_source")||!strcmp(id,"text_gdiplus"));
}
void relay_monitor(State& s,const std::string& key,uint64_t g){
    auto result=sound_signals::read_transcript(key,true,true);
    std::lock_guard lock(s.mutex);
    if(!s.current(g)||!s.config.stt||s.config.stt_source!=key)return;
    if(!result.connected){
        if(s.relay_bound){++s.generation;s.relay_queue.clear();s.pending.clear();s.cv.notify_all();}
        s.relay_bound=false;return;
    }
    // Baseline the current sequence at attachment/activation. Never read
    // a caption which existed before the link became live. Identical
    // phrases with different event sequences are intentional new speech.
    if(!s.relay_bound||result.sequence<s.relay_sequence){
        s.relay_bound=true;s.relay_sequence=result.sequence;return;
    }
    if(result.sequence==s.relay_sequence)return;
    s.relay_sequence=result.sequence;
    if(!result.available||!stts::valid_speech_text(result.text))return;
    if(!local_model(s.config.selected)&&!s.config.stt_allow_cloud)return;
    if(s.relay_queue.size()>=8){
        blog(LOG_WARNING,"[Sound/STTTS] Speech queue full; newest phrase dropped");return;
    }
    s.relay_queue.push_back(std::move(result.text));s.cv.notify_all();
}
void monitor(void* ptr,float){
    auto& s=*static_cast<State*>(ptr);
    if(!s.active.load()||!s.enabled.load())return;
    std::string uuid;uint64_t g;bool relay=false;
    {
        std::lock_guard lock(s.mutex);
        if(s.config.plugin_input)return;
        relay=s.config.stt;
        if(relay){if(s.config.stt_source.empty())return;uuid=s.config.stt_source;}
        else{if(!s.config.monitor||s.config.uuid.empty())return;uuid=s.config.uuid;}
        g=s.generation.load();
        const auto now=std::chrono::steady_clock::now();
        if(now<s.next_poll)return;
        s.next_poll=now+100ms;
    }
    if(relay){relay_monitor(s,uuid,g);return;}
    auto* source=obs_get_source_by_uuid(uuid.c_str());
    std::string text;
    if(source){
        if(text_source(source)&&obs_source_enabled(source)){
            auto* settings=obs_source_get_settings(source);
            // File mode is deliberately excluded; don't read the file on video thread.
            if(!obs_data_get_bool(settings,"read_from_file")&&!obs_data_get_bool(settings,"from_file"))
                text=obs_data_get_string(settings,"text");
            obs_data_release(settings);
        }
        obs_source_release(source);
    }
    std::lock_guard lock(s.mutex);
    if(!s.current(g)||s.config.uuid!=uuid)return;
    const auto now=std::chrono::steady_clock::now();
    if(text!=s.observed){s.observed=std::move(text);s.changed=now;return;}
    if(s.observed!=s.spoken&&now-s.changed>=450ms){
        s.spoken=s.observed;
        if(stts::valid_speech_text(s.observed)){
            s.pending=s.observed;s.cv.notify_all();
        }else if(stts::has_speech(s.observed)){
            blog(LOG_WARNING,"[Sound/TTS] Monitored text rejected: invalid UTF-8 or over %zu bytes",stts::MaxSpeechBytes);
        }
    }
}
void update(void* ptr,obs_data_t* settings){
    auto& s=*static_cast<State*>(ptr);
    Config next{"",obs_data_get_string(settings,"text_source"),
        obs_data_get_string(settings,"text"),obs_data_get_bool(settings,"monitor_text")};
    next.stt=!strcmp(obs_data_get_string(settings,"speech_input"),"stt");
    next.plugin_input=!strcmp(obs_data_get_string(settings,"speech_input"),"plugin");
    next.stt_source=obs_data_get_string(settings,"stt_source");
    next.stt_allow_cloud=obs_data_get_bool(settings,"stt_allow_cloud");
    next.ttl=retention_ttl(settings);
    next.selected=parse_engine(obs_data_get_string(settings,"backend"));
    next.voice=obs_data_get_string(settings,voice_key(next.selected));
    next.synthesis_options=selected_options(settings,next.selected);
    std::lock_guard lock(s.mutex);
    if(next.synthesis_options!=s.config.synthesis_options||next.selected!=s.config.selected||next.voice!=s.config.voice||next.uuid!=s.config.uuid||next.monitor!=s.config.monitor||
       next.stt!=s.config.stt||next.plugin_input!=s.config.plugin_input||next.stt_source!=s.config.stt_source||next.stt_allow_cloud!=s.config.stt_allow_cloud){
        ++s.generation;s.pending.clear();s.relay_queue.clear();
        if(next.stt!=s.config.stt||next.stt_source!=s.config.stt_source)s.relay_bound=false;
        if((next.selected==s.config.selected&&next.voice!=s.config.voice)||next.uuid!=s.config.uuid||next.monitor!=s.config.monitor){
            s.observed.clear();s.spoken.clear();s.changed={};
        }
    }
    s.config=std::move(next);s.cv.notify_all();
}
void activate(void* ptr){
    auto& s=*static_cast<State*>(ptr);std::lock_guard lock(s.mutex);
    s.enabled=obs_source_enabled(s.context);
    s.active=true;s.relay_bound=false;s.relay_queue.clear();s.observed.clear();s.spoken.clear();s.changed={};s.cv.notify_all();
}
void deactivate(void* ptr){
    auto& s=*static_cast<State*>(ptr);std::lock_guard lock(s.mutex);
    s.active=false;++s.generation;s.pending.clear();s.relay_queue.clear();s.relay_bound=false;s.cv.notify_all();
}
void enabled_changed(void* ptr,calldata_t* call){
    auto& s=*static_cast<State*>(ptr);
    std::lock_guard lock(s.mutex);
    s.enabled=calldata_bool(call,"enabled");
    ++s.generation;s.pending.clear();s.relay_queue.clear();s.relay_bound=false;
    s.observed.clear();s.spoken.clear();s.changed={};s.cv.notify_all();
}
void destroy(void* ptr){
    auto* s=static_cast<State*>(ptr);
    signal_handler_disconnect(obs_source_get_signal_handler(s->context),"enable",enabled_changed,s);
    {
        std::lock_guard lock(s->mutex);
        s->stopping=true;++s->generation;s->cv.notify_all();
    }
    if(s->worker.joinable())s->worker.join();
    if(s->lease_worker.joinable())s->lease_worker.join();
    delete s;
}
bool speak(obs_properties_t*,obs_property_t*,void* ptr){
    auto* s=static_cast<State*>(ptr);if(!s)return false;
    auto* settings=obs_source_get_settings(s->context);
    std::string text=obs_data_get_string(settings,"text");obs_data_release(settings);
    std::lock_guard lock(s->mutex);
    if(s->config.stt||s->config.plugin_input)return false;
    if(!s->active.load()||!s->enabled.load()){
        blog(LOG_WARNING,"[Sound/TTS] Speak requires an active, enabled source");return false;
    }
    if(!stts::valid_speech_text(text)){
        blog(LOG_WARNING,"[Sound/TTS] Speak rejected: enter nonempty UTF-8 text up to %zu bytes (received %zu)",stts::MaxSpeechBytes,text.size());
        return false;
    }
    s->pending=std::move(text);s->cv.notify_all();return false;
}
bool stop(obs_properties_t*,obs_property_t*,void* ptr){
    auto* s=static_cast<State*>(ptr);if(!s)return false;
    std::lock_guard lock(s->mutex);
    ++s->generation;s->pending.clear();s->relay_queue.clear();s->cv.notify_all();return false;
}
bool list_texts(void* ptr,obs_source_t* source){
    if(text_source(source)){
        auto* p=static_cast<obs_property_t*>(ptr);
        obs_property_list_add_string(p,obs_source_get_name(source),obs_source_get_uuid(source));
    }
    return true;
}
void speech_input_visibility(obs_properties_t* p,bool relay,bool plugin){
    for(auto key:{"text_source","monitor_text","text","speak"})
        obs_property_set_visible(obs_properties_get(p,key),!relay&&!plugin);
    obs_property_set_visible(obs_properties_get(p,"stt_source"),relay);
    for(auto key:{"stt_allow_cloud","stt_note"})
        obs_property_set_visible(obs_properties_get(p,key),relay||plugin);
    obs_property_set_visible(obs_properties_get(p,"plugin_note"),plugin);
}
bool speech_input_changed(obs_properties_t* p,obs_property_t*,obs_data_t* d){
    std::string mode=obs_data_get_string(d,"speech_input");
    speech_input_visibility(p,mode=="stt",mode=="plugin");return true;
}
void add_voices(obs_properties_t* p,Engine e,const char* key){
    auto* voice=obs_properties_add_list(p,key,"Voice",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    if(e==Engine::Kokoro||e==Engine::KokoroNative){for(auto v:stts::KokoroVoices)obs_property_list_add_string(voice,v.label,v.id);}
    else if(e==Engine::TikTok){for(auto v:stts::TikTokVoices)obs_property_list_add_string(voice,v.label,v.id);}
    else if(e==Engine::Edge){for(auto v:stts::EdgeVoices)obs_property_list_add_string(voice,v.label,v.id);}
    else obs_property_list_add_string(voice,e==Engine::Piper?"Amy (English US)":e==Engine::ChatterboxNative?"Embedded Turbo voice":"Cached Nano built-in voice",default_voice(e));
}
void show_backend(obs_properties_t* p,Engine selected){
    for(auto e:backends)obs_property_set_visible(obs_properties_get(p,voice_key(e)),e==selected);
    obs_property_set_visible(obs_properties_get(p,"cloud_note"),selected==Engine::TikTok||selected==Engine::Edge);
}
void retention_visibility(obs_properties_t* p,Engine e,int mode){
    if(auto* field=obs_properties_get(p,"retention_mode"))obs_property_set_visible(field,local_model(e));
    if(auto* field=obs_properties_get(p,"retention_ttl"))obs_property_set_visible(field,local_model(e)&&mode==1);
}
bool backend_changed(obs_properties_t* p,obs_property_t*,obs_data_t* settings){
    const auto e=parse_engine(obs_data_get_string(settings,"backend"));
    show_backend(p,e);
    option_visibility(p,e,settings);
    retention_visibility(p,e,int(obs_data_get_int(settings,"retention_mode")));
    return true;
}
obs_properties_t* props(void* ptr){
    auto* p=obs_properties_create();
    {
        auto* selector=obs_properties_add_list(p,"backend","Speech backend",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
        for(auto e:backends){
            obs_property_list_add_string(selector,engine_label(e),model(e));
            add_voices(p,e,voice_key(e));
        }
        auto* cloud=obs_properties_add_text(p,"cloud_note","Cloud: TikTok sends selected text directly to Ottsy (ottsy.weilbyte.dev), a third-party community proxy; Edge sends selected text to Microsoft online speech. No automatic fallback.",OBS_TEXT_INFO);
        (void)cloud;
        Engine selected=Engine::Kokoro;
        if(ptr){
            auto& s=*static_cast<State*>(ptr);std::lock_guard lock(s.mutex);
            selected=s.config.selected;
        }
        if(selected==Engine::Invalid){
            auto* settings=ptr?obs_source_get_settings(static_cast<State*>(ptr)->context):nullptr;
            if(settings){
                auto index=obs_property_list_add_string(selector,"Saved backend (unavailable)",obs_data_get_string(settings,"backend"));
                obs_property_list_item_disable(selector,index,true);
                obs_data_release(settings);
            }
        }
        show_backend(p,selected);
        obs_property_set_modified_callback(selector,backend_changed);
    }
    add_options(p);
    {
        auto* settings=ptr?obs_source_get_settings(static_cast<State*>(ptr)->context):obs_data_create();
        if(!ptr)option_defaults(settings);
        const auto selected=ptr?parse_engine(obs_data_get_string(settings,"backend")):Engine::Kokoro;
        option_visibility(p,selected,settings);obs_data_release(settings);
    }
    {
        auto* policy=obs_properties_add_list(p,"retention_mode","Local model retention",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(policy,"Unload when unused",0);
        obs_property_list_add_int(policy,"Idle timeout (TTL)",1);
        obs_property_list_add_int(policy,"Keep warm",2);
        auto* ttl=obs_properties_add_int(p,"retention_ttl","Idle TTL (seconds)",1,3600,1);
        obs_property_int_set_suffix(ttl," s");
        obs_property_set_long_description(policy,"Models load on first speech. Active enabled sources share a model; eviction begins after the last source releases it. Keep warm lasts until worker restart or this source releases with a different policy.");
        obs_property_set_modified_callback(policy,[](obs_properties_t* p,obs_property_t*,obs_data_t* d){
            const auto e=parse_engine(obs_data_get_string(d,"backend"));
            retention_visibility(p,e,int(obs_data_get_int(d,"retention_mode")));return true;
        });
        auto* settings=ptr?obs_source_get_settings(static_cast<State*>(ptr)->context):nullptr;
        const auto e=settings?parse_engine(obs_data_get_string(settings,"backend")):Engine::Kokoro;
        retention_visibility(p,e,settings?int(obs_data_get_int(settings,"retention_mode")):0);
        if(settings)obs_data_release(settings);
    }
    auto* input=obs_properties_add_list(p,"speech_input","Speech input",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(input,"Manual / text source","text");
    obs_property_list_add_string(input,"STT filter (speech-to-speech)","stt");
    obs_property_list_add_string(input,"Other plugin (text signal)","plugin");
    obs_property_set_modified_callback(input,speech_input_changed);
    auto* stt=obs_properties_add_list(p,"stt_source","Transcription filter",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(stt,"None","");
    for(const auto& endpoint:sound_signals::discover()){
        if(endpoint.kind!=sound_signals::Kind::Transcript||!endpoint.relay_safe)continue;
        obs_property_list_add_string(stt,endpoint.label.c_str(),endpoint.channel.c_str());
    }
    obs_properties_add_text(p,"plugin_note","Other native plugins can submit text to this destination through the versioned Sound signal API. No inference or cloud request until a message is accepted.",OBS_TEXT_INFO);
    obs_properties_add_bool(p,"stt_allow_cloud","Allow sending transcribed speech to cloud TTS");
    obs_properties_add_text(p,"stt_note","Speaks new finalized phrases or accepted plugin messages in order (up to 8 waiting). Use a microphone or isolated input, not audio containing this TTS output. Wear headphones or disable output monitoring to avoid acoustic feedback. Cloud relay requires explicit permission.",OBS_TEXT_INFO);
    auto* list=obs_properties_add_list(p,"text_source","OBS text source",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(list,"None","");
    obs_enum_sources(list_texts,list);
    if(ptr){
        auto& s=*static_cast<State*>(ptr);std::lock_guard lock(s.mutex);
        bool found=false;
        for(size_t i=0;i<obs_property_list_item_count(list);++i)
            found|=s.config.uuid==obs_property_list_item_string(list,i);
        if(!found){
            auto i=obs_property_list_add_string(list,"Saved text source (unavailable)",s.config.uuid.c_str());
            obs_property_list_item_disable(list,i,true);
        }
    }
    obs_properties_add_bool(p,"monitor_text","Speak changed text automatically");
    obs_properties_add_text(p,"text","Manual speech (up to 16 KiB; split automatically)",OBS_TEXT_MULTILINE);
    obs_properties_add_button2(p,"speak","Speak",speak,ptr);
    obs_properties_add_button2(p,"stop","Stop speech",stop,ptr);
    bool relay=false,plugin=false;
    if(ptr){
        auto& s=*static_cast<State*>(ptr);std::lock_guard lock(s.mutex);relay=s.config.stt;plugin=s.config.plugin_input;
        bool found=false;
        for(size_t i=0;i<obs_property_list_item_count(stt);++i)found|=s.config.stt_source==obs_property_list_item_string(stt,i);
        if(!found){auto i=obs_property_list_add_string(stt,"Saved transcription filter (unavailable)",s.config.stt_source.c_str());obs_property_list_item_disable(stt,i,true);}
    }
    speech_input_visibility(p,relay,plugin);

    obs_properties_add_text(p,"note",
        "Switch backends without recreating this source. Each backend remembers its voice. No automatic fallback.",
        OBS_TEXT_INFO);
    return p;
}
void defaults(obs_data_t* settings){
    obs_data_set_default_string(settings,"backend","kokoro");
    for(auto e:backends)obs_data_set_default_string(settings,voice_key(e),default_voice(e));
    option_defaults(settings);
    obs_data_set_default_string(settings,"text","");
    obs_data_set_default_string(settings,"text_source","");
    obs_data_set_default_bool(settings,"monitor_text",false);
    obs_data_set_default_string(settings,"speech_input","text");
    obs_data_set_default_string(settings,"stt_source","");
    obs_data_set_default_bool(settings,"stt_allow_cloud",false);
    obs_data_set_default_int(settings,"retention_mode",0);
    obs_data_set_default_int(settings,"retention_ttl",30);
}
void status(void* p,calldata_t* call){
    auto& s=*static_cast<State*>(p);
    calldata_set_int(call,"completed",s.completed.load());
    calldata_set_int(call,"failed",s.failed.load());
}
// Explicit opt-in destination. Callers own event deduplication; acceptance
// only means queued, not successful synthesis. No I/O on the calling thread.
void enqueue_signal(void* ptr,calldata_t* call){
    calldata_set_bool(call,"accepted",false);
    auto& s=*static_cast<State*>(ptr);
    const char* raw=calldata_string(call,"text");
    if(!raw||strnlen(raw,stts::MaxSpeechBytes+1)>stts::MaxSpeechBytes)return;
    std::string text(raw);
    if(!stts::valid_speech_text(text))return;
    std::lock_guard lock(s.mutex);
    if(!s.config.plugin_input||!s.active||!s.enabled||s.stopping||
       s.config.selected==Engine::Invalid||(!local_model(s.config.selected)&&!s.config.stt_allow_cloud)||
       s.relay_queue.size()>=8)return;
    s.relay_queue.push_back(std::move(text));s.cv.notify_all();
    calldata_set_bool(call,"accepted",true);
}
void* create(obs_data_t* settings,obs_source_t* context){
    auto* s=new State(context);
    try {
        update(s,settings);
        s->worker=std::thread(run,s);
        s->lease_worker=std::thread(maintain_leases,s);
    }catch(...){
        {std::lock_guard lock(s->mutex);s->stopping=true;s->cv.notify_all();}
        if(s->worker.joinable())s->worker.join();
        if(s->lease_worker.joinable())s->lease_worker.join();
        delete s;return nullptr;
    }
    // libobs sets enabled=true AFTER calling create; activation and
    // the enable signal observe the initialized source instead.
    signal_handler_connect(obs_source_get_signal_handler(context),"enable",enabled_changed,s);
    proc_handler_add(obs_source_get_proc_handler(context),
                     "void sound_tts_status_v1(out int completed, out int failed)",status,s);
    proc_handler_add(obs_source_get_proc_handler(context),"void sound_tts_enqueue_v1(in string text, out bool accepted)",enqueue_signal,s);
    sound_signals::register_broker(context);
    return s;
}
const char* name(void*){return "LLL.sound TTS";}
obs_source_info info(){
    obs_source_info x{};
    x.id="sound_tts_source";x.type=OBS_SOURCE_TYPE_INPUT;x.output_flags=OBS_SOURCE_AUDIO;
    x.get_name=name;x.create=create;x.destroy=destroy;x.get_defaults=defaults;
    x.get_properties=props;x.update=update;x.activate=activate;x.deactivate=deactivate;
    x.video_tick=monitor; // text event monitoring only, never audio output
    return x;
}
}
void register_sound_tts(){
    curl_global_init(CURL_GLOBAL_DEFAULT);
    static auto unified=info();
    obs_register_source(&unified);
    blog(LOG_INFO,"[Sound/TTS] SOUND_TTS_UNIFIED_20261118_U481");
    blog(LOG_INFO,"[Sound/TTS] SOUND_TTS_EDGE_20261118_E521");
    blog(LOG_INFO,"[Sound/TTS] SOUND_TTS_RETENTION_20261118_R621");
    blog(LOG_INFO,"[Sound/TTS] SOUND_TTS_LONG_INPUT_20261118_L673");
    blog(LOG_INFO,"[Sound/TTS] SOUND_TTS_NATIVE_OPTIONS_20261118_O536");
    blog(LOG_INFO,"[Sound/TTS] SOUND_STTTS_RELAY_20261120_R820");
    blog(LOG_INFO,"[Sound/Signals] SOUND_SIGNAL_INTEROP_20261120_I421");
}
