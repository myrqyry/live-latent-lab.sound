#pragma once
#include "stt_whisper.hpp"
#include "stt_parakeet.hpp"
#include "stt_nemotron.hpp"
#ifdef SOUND_WITH_ZIPFORMER
#include "stt_zipformer.hpp"
#endif
#include "framework/stt_config.hpp"
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace stts {
// Worker-owned session. Native engines keep their own runtime ABI; capture,
// caption publication, and cancellation epochs remain in the OBS filter.
class SttBackend {
    enum class Kind { None, Whisper, Parakeet, Nemotron, Zipformer };
    Kind kind_=Kind::None;
    SttWhisper whisper_;
    SttParakeet parakeet_;
    SttNemotron nemotron_;
#ifdef SOUND_WITH_ZIPFORMER
    SttZipformer zipformer_;
#endif
    bool started_=false;
    size_t fed_=0;
    size_t chunk()const{return kind_==Kind::Zipformer?1600:8960;}
public:
    using Abort=bool(*)(void*);
    void close(){
        reset();whisper_.close();parakeet_.close();nemotron_.close();kind_=Kind::None;
#ifdef SOUND_WITH_ZIPFORMER
        zipformer_.close();
#endif
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
#ifdef SOUND_WITH_ZIPFORMER
        }else if(c.backend=="zipformer"){
            if(!zipformer_.open(runtime/"zipformer/libsound-zipformer-sherpa.so",c.zipformer_model,c.zipformer_threads,c.zipformer_decode))return false;
            kind_=Kind::Zipformer;
#endif
        }else return false;
        return true;
    }
    std::string device()const{
        switch(kind_){
        case Kind::Whisper:return whisper_.device();
        case Kind::Parakeet:return parakeet_.delegate();
        case Kind::Nemotron:return nemotron_.device_backend();
        case Kind::Zipformer:return "CPU";
        default:return "none";
        }
    }
    bool streaming()const{return kind_==Kind::Nemotron||kind_==Kind::Zipformer;}
    bool partial_ready(size_t count)const{return streaming()&&count>=fed_&&count-fed_>=chunk();}
    void reset(){
        if(kind_==Kind::Nemotron)nemotron_.reset();
#ifdef SOUND_WITH_ZIPFORMER
        if(kind_==Kind::Zipformer)zipformer_.reset();
#endif
        started_=false;fed_=0;
    }
    bool advance(const std::vector<float>& pcm,bool final,Abort abort,void* opaque,std::string& out){
        out.clear();
        if(!streaming()||pcm.size()<fed_||abort(opaque))return false;
        if(!started_){
#ifdef SOUND_WITH_ZIPFORMER
            if(kind_==Kind::Zipformer){if(!zipformer_.begin())return false;}
            else
#endif
            if(!nemotron_.begin())return false;
            started_=true;fed_=0;
        }
        while(pcm.size()-fed_>=chunk()||(final&&fed_<pcm.size())){
            if(abort(opaque))return false;
            const auto n=std::min(chunk(),pcm.size()-fed_);
#ifdef SOUND_WITH_ZIPFORMER
            if(kind_==Kind::Zipformer){if(!zipformer_.feed(pcm.data()+fed_,n,out,abort,opaque))return false;}
            else
#endif
            if(!nemotron_.feed(pcm.data()+fed_,n,out))return false;
            fed_+=n;
        }
        if(abort(opaque))return false;
#ifdef SOUND_WITH_ZIPFORMER
        if(kind_==Kind::Zipformer)return (!final||zipformer_.finish(out,abort,opaque))&&!abort(opaque);
#endif
        return (!final||nemotron_.finish(out))&&!abort(opaque);
    }
    bool finalize(std::vector<float>& pcm,const SttConfig& c,Abort abort,void* opaque,std::string& out){
        out.clear();
        if(abort(opaque)||pcm.empty())return false;
        if(streaming())return advance(pcm,true,abort,opaque,out);
        if(pcm.size()<16000)pcm.resize(16000,0.f);
        bool ok=false;
        if(kind_==Kind::Whisper)ok=whisper_.transcribe(pcm,c.threads,c.language,c.translate,c.prompt,abort,opaque,out,c.whisper_decode);
        else if(kind_==Kind::Parakeet)ok=parakeet_.transcribe(pcm,out);
        return ok&&!abort(opaque);
    }
};
} // namespace stts
