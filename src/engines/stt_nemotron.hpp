#pragma once
#include <dlfcn.h>
#include <array>
#include <string>
#include <filesystem>
namespace stts {
class SttNemotron {
    void* lib_=nullptr;
    void* session_=nullptr;
    int (*abi_)()=nullptr;
    void* (*open_)(const char*,int,const char*,int)=nullptr;
    void (*close_)(void*)=nullptr;
    int (*begin_)(void*)=nullptr;
    void (*reset_)(void*)=nullptr;
    int (*feed_)(void*,const float*,int)=nullptr;
    int (*finish_)(void*)=nullptr;
    int (*text_)(void*,char*,size_t)=nullptr;
    template<class T> bool resolve(T& fn,const char* name){
        fn=reinterpret_cast<T>(dlsym(lib_,name));return fn!=nullptr;
    }
    bool text(std::string& out){
        std::array<char,8193> buf{};
        if(!text_(session_,buf.data(),buf.size()))return false;
        out=buf.data();return true;
    }
public:
    SttNemotron()=default;
    SttNemotron(const SttNemotron&)=delete;
    ~SttNemotron(){close();}
    void close(){
        if(session_)close_(session_);
        session_=nullptr;
        if(lib_)dlclose(lib_);
        lib_=nullptr;
    }
    bool open(const std::string& lib,const std::string& model,int threads,const std::string& language,int right){
        close();
        std::error_code ec;
        if(!std::filesystem::is_regular_file(model,ec)||std::filesystem::path(model).extension()!=".gguf")return false;
        lib_=dlopen(lib.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_NODELETE);
        if(!lib_)return false;
        if(!resolve(abi_,"sound_nemotron_abi")||abi_()!=1||
           !resolve(open_,"sound_nemotron_open")||!resolve(close_,"sound_nemotron_close")||
           !resolve(begin_,"sound_nemotron_begin")||!resolve(reset_,"sound_nemotron_reset")||
           !resolve(feed_,"sound_nemotron_feed")||!resolve(finish_,"sound_nemotron_finish")||
           !resolve(text_,"sound_nemotron_text")){close();return false;}
        session_=open_(model.c_str(),threads,language.c_str(),right);
        if(!session_){close();return false;}
        return true;
    }
    bool begin(){return session_&&begin_(session_);}
    void reset(){if(session_)reset_(session_);}
    bool feed(const float* pcm,size_t count,std::string& out){
        return session_&&count&&count<=17920&&feed_(session_,pcm,int(count))&&text(out);
    }
    bool finish(std::string& out){return session_&&finish_(session_)&&text(out);}
};
}
