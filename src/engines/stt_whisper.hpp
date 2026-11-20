#pragma once
#include <whisper.h>
#include <dlfcn.h>
#include <string>
#include <vector>
namespace stts {
// Explicit qualified CPU runtime, loaded only while a filter uses it.
class SttWhisper {
    void* lib_=nullptr;
    whisper_context* ctx_=nullptr;
    template<class T> bool symbol(T& target,const char* name){
        target=reinterpret_cast<T>(dlsym(lib_,name));return target!=nullptr;
    }
#define STT_API(n) decltype(&n) n##_=nullptr
    STT_API(whisper_context_default_params);
    STT_API(whisper_init_from_file_with_params);
    STT_API(whisper_free);
    STT_API(whisper_full_default_params);
    STT_API(whisper_full);
    STT_API(whisper_full_n_segments);
    STT_API(whisper_full_get_segment_text);
    STT_API(whisper_is_multilingual);
    STT_API(whisper_lang_id);
#undef STT_API
public:
    SttWhisper()=default;
    SttWhisper(const SttWhisper&)=delete;
    ~SttWhisper(){close();}
    void close(){
        if(ctx_)whisper_free_(ctx_);
        ctx_=nullptr;
        if(lib_)dlclose(lib_);
        lib_=nullptr;
    }
    bool open(const std::string& lib,const std::string& model,const std::string& lang,bool translate){
        close();
        if(model.empty())return false;
        lib_=dlopen(lib.c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!lib_)return false;
#define STT_RESOLVE(n) if(!symbol(n##_,#n)){close();return false;}
        STT_RESOLVE(whisper_context_default_params)
        STT_RESOLVE(whisper_init_from_file_with_params)
        STT_RESOLVE(whisper_free)
        STT_RESOLVE(whisper_full_default_params)
        STT_RESOLVE(whisper_full)
        STT_RESOLVE(whisper_full_n_segments)
        STT_RESOLVE(whisper_full_get_segment_text)
        STT_RESOLVE(whisper_is_multilingual)
        STT_RESOLVE(whisper_lang_id)
#undef STT_RESOLVE
        if(lang!="auto"&&whisper_lang_id_(lang.c_str())<0){close();return false;}
        auto p=whisper_context_default_params_();p.use_gpu=false;
        ctx_=whisper_init_from_file_with_params_(model.c_str(),p);
        if(!ctx_||(!whisper_is_multilingual_(ctx_)&&(translate||(lang!="auto"&&lang!="en")))){
            close();return false;
        }
        return true;
    }
    bool transcribe(const std::vector<float>& pcm,int threads,const std::string& lang,bool translate,
                    const std::string& prompt,bool(*abort)(void*),void* opaque,std::string& out){
        if(!ctx_||pcm.empty())return false;
        auto p=whisper_full_default_params_(WHISPER_SAMPLING_GREEDY);
        p.n_threads=threads;p.language=lang.c_str();p.translate=translate;
        p.no_context=true;p.no_timestamps=true;p.print_progress=false;p.print_special=false;
        p.print_realtime=false;p.print_timestamps=false;
        p.initial_prompt=prompt.empty()?nullptr:prompt.c_str();
        p.abort_callback=abort;p.abort_callback_user_data=opaque;
        if(whisper_full_(ctx_,p,pcm.data(),int(pcm.size()))!=0)return false;
        out.clear();
        for(int i=0;i<whisper_full_n_segments_(ctx_);++i){
            const char* t=whisper_full_get_segment_text_(ctx_,i);
            if(t)out+=t;
            if(out.size()>8192)return false;
        }
        const auto first=out.find_first_not_of(" \t\r\n");
        if(first==std::string::npos)out.clear();
        else out=out.substr(first,out.find_last_not_of(" \t\r\n")-first+1);
        return true;
    }
};
}
