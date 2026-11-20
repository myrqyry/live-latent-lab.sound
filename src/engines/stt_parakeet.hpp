#pragma once
// ABI from the official MediaPipe 1.2.3 wheel's audio_transcriber.py
// and base_options_c.py. Same pinned BaseOptions as the YAMNet engine.
#include "mediapipe_audio_123.hpp"
#include <filesystem>
#include <cstring>
namespace stts {
class SttParakeet {
    struct Segment {const char* transcript;bool is_final;int64_t from_ms,to_ms;};
    struct Result {Segment* segments;uint32_t count;};
    struct Options {
        mp_audio_123::BaseOptions base_options;
        int sample_rate_hz=16000;
        bool interim=false;
        int architecture=0;
        const char* prompt=nullptr;
        const char* cache_dir=nullptr;
    };
    using Create=int(*)(Options*,void**,char**);
    using Transcribe=int(*)(void*,const float*,size_t,bool,Result*,char**);
    using CloseResult=void(*)(Result*);
    using Close=int(*)(void*,char**);
    using FreeError=void(*)(char*);
    void* lib_=nullptr,*handle_=nullptr;
    Create create_=nullptr;
    Transcribe transcribe_=nullptr;
    CloseResult close_result_=nullptr;
    Close close_=nullptr;
    FreeError free_error_=nullptr;
    std::string error_;
    bool check(int status,char* message){
        if(status)error_=message?message:"MediaPipe status "+std::to_string(status);
        if(message&&free_error_)free_error_(message);
        return status==0;
    }
public:
    SttParakeet()=default;
    SttParakeet(const SttParakeet&)=delete;
    ~SttParakeet(){close();}
    const std::string& error()const{return error_;}
    void close(){
        if(handle_){char* err=nullptr;const auto rc=close_(handle_,&err);check(rc,err);handle_=nullptr;}
        if(lib_)dlclose(lib_);
        lib_=nullptr;create_=nullptr;transcribe_=nullptr;close_result_=nullptr;close_=nullptr;free_error_=nullptr;
    }
    bool open(const std::string& lib,const std::string& model){
        close();error_.clear();
        if(!std::filesystem::is_regular_file(model)||std::filesystem::path(model).extension()!=".litertlm"){
            error_="Select a local .litertlm model";return false;
        }
        lib_=dlopen(lib.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_NODELETE);
        if(!lib_){error_="MediaPipe runtime unavailable";return false;}
        create_=reinterpret_cast<Create>(dlsym(lib_,"MpAudioTranscriberCreate"));
        transcribe_=reinterpret_cast<Transcribe>(dlsym(lib_,"MpAudioTranscriberTranscribe"));
        close_result_=reinterpret_cast<CloseResult>(dlsym(lib_,"MpAudioTranscriberCloseResult"));
        close_=reinterpret_cast<Close>(dlsym(lib_,"MpAudioTranscriberClose"));
        free_error_=reinterpret_cast<FreeError>(dlsym(lib_,"MpErrorFree"));
        if(!create_||!transcribe_||!close_result_||!close_||!free_error_){error_="Missing MediaPipe 1.2.3 AudioTranscriber API";close();return false;}
        Options options{};
        options.base_options.model_asset_path=model.c_str();
        options.base_options.delegate=1; // CPU
        options.base_options.host_system=1;
        char* err=nullptr;
        auto status=create_(&options,&handle_,&err);
        if(!check(status,err)||!handle_){close();return false;}
        return true;
    }
    bool transcribe(const std::vector<float>& pcm,std::string& text){
        text.clear();error_.clear();
        if(!handle_||pcm.empty()||pcm.size()>240000){error_="Invalid PCM";return false;}
        Result result{};char* err=nullptr;
        auto status=transcribe_(handle_,pcm.data(),pcm.size(),true,&result,&err);
        const bool ok=check(status,err);
        bool valid=ok&&result.count<=2048&&(!result.count||result.segments);
        if(valid){
            for(uint32_t i=0;i<result.count;++i){
                auto& seg=result.segments[i];
                if(!seg.is_final||!seg.transcript)continue;
                const auto size=strnlen(seg.transcript,8193);
                if(size>8192||text.size()+size+1>8192){valid=false;break;}
                if(!size)continue;
                if(!text.empty())text+=' ';
                text.append(seg.transcript,size);
            }
        }
        close_result_(&result);
        if(!valid){text.clear();if(error_.empty())error_="Invalid transcription result";}
        return valid;
    }
};
}
