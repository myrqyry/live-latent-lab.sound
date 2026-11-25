#pragma once
#include "stt_whisper.hpp"
#include "stt_parakeet.hpp"
#include "stt_nemotron.hpp"
#include "framework/stt_config.hpp"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace stts {
// Worker-owned session. Native engines keep their own runtime ABI; capture,
// caption publication, and cancellation epochs remain in the OBS filter.
class SttBackend {
    enum class Kind { None, Whisper, Parakeet, Nemotron };
    Kind kind_=Kind::None;
    SttWhisper whisper_;
    SttParakeet parakeet_;
    SttNemotron nemotron_;
    bool started_=false;
    size_t fed_=0;
    static constexpr size_t chunk_=8960;
public:
    using Abort=bool(*)(void*);
    void close(){
        reset();whisper_.close();parakeet_.close();nemotron_.close();kind_=Kind::None;
    }
    bool open(const SttConfig& c,const std::filesystem::path& runtime){
        close();
        if(c.backend=="whisper"){
            if(!whisper_.open((runtime/"libwhisper.so.1").string(),c.model,c.language,c.translate,c.whisper_device))return false;
            kind_=Kind::Whisper;
        }else if(c.backend=="parakeet"){
            if(!parakeet_.open(c.parakeet_library,c.parakeet_model,c.parakeet_device))return false;
            kind_=Kind::Parakeet;
        }else if(c.backend=="nemotron"){
            const auto lib=c.nemotron_library.empty()?(runtime/"libsound-nemotron-runtime.so").string():c.nemotron_library;
            auto attempt=[&](const std::string& device){
                return nemotron_.open(lib,c.nemotron_model,c.nemotron_threads,c.nemotron_language,c.nemotron_right,device)&&nemotron_.begin();
            };
            if(!attempt(c.nemotron_device)&&!(c.nemotron_device=="auto"&&attempt("cpu"))){
                nemotron_.close();return false;
            }
            nemotron_.reset();kind_=Kind::Nemotron;
        }else return false;
        return true;
    }
    std::string device()const{
        switch(kind_){
        case Kind::Whisper:return whisper_.device();
        case Kind::Parakeet:return parakeet_.delegate();
        case Kind::Nemotron:return nemotron_.device_backend();
        default:return "none";
        }
    }
    bool streaming()const{return kind_==Kind::Nemotron;}
    bool partial_ready(size_t count)const{return streaming()&&count>=fed_&&count-fed_>=chunk_;}
    void reset(){
        if(streaming())nemotron_.reset();
        started_=false;fed_=0;
    }
    bool advance(const std::vector<float>& pcm,bool final,Abort abort,void* opaque,std::string& out){
        out.clear();
        if(!streaming()||pcm.size()<fed_||abort(opaque))return false;
        if(!started_){if(!nemotron_.begin())return false;started_=true;fed_=0;}
        while(pcm.size()-fed_>=chunk_||(final&&fed_<pcm.size())){
            if(abort(opaque))return false;
            const auto n=std::min(chunk_,pcm.size()-fed_);
            if(!nemotron_.feed(pcm.data()+fed_,n,out))return false;
            fed_+=n;
        }
        if(abort(opaque))return false;
        return (!final||nemotron_.finish(out))&&!abort(opaque);
    }
    bool finalize(std::vector<float>& pcm,const SttConfig& c,Abort abort,void* opaque,std::string& out){
        out.clear();
        if(abort(opaque)||pcm.empty())return false;
        if(streaming())return advance(pcm,true,abort,opaque,out);
        if(pcm.size()<16000)pcm.resize(16000,0.f);
        bool ok=false;
        if(kind_==Kind::Whisper)ok=whisper_.transcribe(pcm,c.threads,c.language,c.translate,c.prompt,abort,opaque,out);
        else if(kind_==Kind::Parakeet)ok=parakeet_.transcribe(pcm,out);
        return ok&&!abort(opaque);
    }
};
} // namespace stts
