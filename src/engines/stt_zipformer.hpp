#pragma once
#include <c-api.h>
#include "framework/stt_config.hpp"
#include <openssl/evp.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <array>
#include <string>
#include <memory>

namespace stts {
// This first opt-in backend accepts only the qualified model set. Sherpa
// can terminate its host on malformed model metadata: do not feed it
// arbitrary unchecked ONNX files.
class SttZipformer {
    void* lib_=nullptr;
    const SherpaOnnxOnlineRecognizer* recognizer_=nullptr;
    const SherpaOnnxOnlineStream* stream_=nullptr;
    decltype(&SherpaOnnxGetVersionStr) version_=nullptr;
    decltype(&SherpaOnnxCreateOnlineRecognizer) create_=nullptr;
    decltype(&SherpaOnnxDestroyOnlineRecognizer) destroy_=nullptr;
    decltype(&SherpaOnnxCreateOnlineStream) stream_create_=nullptr;
    decltype(&SherpaOnnxDestroyOnlineStream) stream_destroy_=nullptr;
    decltype(&SherpaOnnxOnlineStreamAcceptWaveform) feed_=nullptr;
    decltype(&SherpaOnnxIsOnlineStreamReady) ready_=nullptr;
    decltype(&SherpaOnnxDecodeOnlineStream) decode_=nullptr;
    decltype(&SherpaOnnxGetOnlineStreamResult) result_=nullptr;
    decltype(&SherpaOnnxDestroyOnlineRecognizerResult) release_=nullptr;
    decltype(&SherpaOnnxOnlineStreamInputFinished) finish_=nullptr;

    template<class T> bool symbol(T& fn,const char* name){
        fn=reinterpret_cast<T>(dlsym(lib_,name));return fn!=nullptr;
    }
    static bool verified(const std::filesystem::path& path,uintmax_t bytes,const char* sha){
        std::error_code ec;
        if(!std::filesystem::is_regular_file(path,ec)||std::filesystem::file_size(path,ec)!=bytes||ec)return false;
        std::ifstream f(path,std::ios::binary);
        if(!f)return false;
        std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
        if(!ctx||EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1)return false;
        std::array<char,65536> b{};
        while(f.read(b.data(),b.size())||f.gcount())
            if(EVP_DigestUpdate(ctx.get(),b.data(),size_t(f.gcount()))!=1)return false;
        if(!f.eof()||f.bad())return false;
        unsigned char digest[EVP_MAX_MD_SIZE];unsigned len=0;
        if(EVP_DigestFinal_ex(ctx.get(),digest,&len)!=1||len!=32)return false;
        static constexpr char hex[]="0123456789abcdef";
        std::string got;got.reserve(64);
        for(unsigned i=0;i<len;i++){got+=hex[digest[i]>>4];got+=hex[digest[i]&15];}
        return got==sha;
    }
    bool read(std::string& out,bool(*abort)(void*),void* opaque){
        if(!stream_)return false;
        while(ready_(recognizer_,stream_)){
            if(abort(opaque))return false;
            decode_(recognizer_,stream_);
        }
        if(abort(opaque))return false;
        auto* r=result_(recognizer_,stream_);
        if(!r)return false;
        struct Guard {decltype(release_) fn;const SherpaOnnxOnlineRecognizerResult* r;~Guard(){fn(r);}} guard{release_,r};
        out=r->text?r->text:"";
        return true;
    }
public:
    ~SttZipformer(){close();}
    SttZipformer()=default;
    SttZipformer(const SttZipformer&)=delete;
    SttZipformer& operator=(const SttZipformer&)=delete;
    void reset(){if(stream_){stream_destroy_(stream_);stream_=nullptr;}}
    void close(){reset();if(recognizer_){destroy_(recognizer_);recognizer_=nullptr;}if(lib_){dlclose(lib_);lib_=nullptr;}}
    bool open(const std::filesystem::path& library,const std::filesystem::path& dir,int threads,const ZipformerDecode& options={}){
        close();
        const auto encoder=dir/"encoder-epoch-99-avg-1.int8.onnx",decoder=dir/"decoder-epoch-99-avg-1.int8.onnx",
            joiner=dir/"joiner-epoch-99-avg-1.int8.onnx",tokens=dir/"tokens.txt";
        if(dir.empty()||
           !verified(encoder,42845182,"3810755ce7c3ab26b42a8bcf39d191308fa27fb0f53358823ba46141d03b7eb3")||
           !verified(decoder,539499,"21e2a2acd961b3ac72f55be2f10f1a285e1b0b0ba010d7c0b6eab141411b163c")||
           !verified(joiner,259572,"e085d73b593cf9b0707f370dbd656d58327d3fe36d80d849202ef81df02cb01e")||
           !verified(tokens,5048,"49e3c2646595fd907228b3c6787069658f67b17377c60aeb8619c4551b2316fb"))return false;
        lib_=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL|RTLD_DEEPBIND|RTLD_NODELETE);
        if(!lib_)return false;
        if(!symbol(version_,"SherpaOnnxGetVersionStr")||
           !symbol(create_,"SherpaOnnxCreateOnlineRecognizer")||!symbol(destroy_,"SherpaOnnxDestroyOnlineRecognizer")||
           !symbol(stream_create_,"SherpaOnnxCreateOnlineStream")||!symbol(stream_destroy_,"SherpaOnnxDestroyOnlineStream")||
           !symbol(feed_,"SherpaOnnxOnlineStreamAcceptWaveform")||!symbol(ready_,"SherpaOnnxIsOnlineStreamReady")||
           !symbol(decode_,"SherpaOnnxDecodeOnlineStream")||!symbol(result_,"SherpaOnnxGetOnlineStreamResult")||
           !symbol(release_,"SherpaOnnxDestroyOnlineRecognizerResult")||!symbol(finish_,"SherpaOnnxOnlineStreamInputFinished")||
           std::string(version_())!="1.12.40"){close();return false;}
        SherpaOnnxOnlineRecognizerConfig c{};
        c.feat_config={16000,80};c.model_config.transducer={encoder.c_str(),decoder.c_str(),joiner.c_str()};
        c.model_config.tokens=tokens.c_str();c.model_config.provider="cpu";c.model_config.num_threads=threads;
        c.decoding_method=options.beam?"modified_beam_search":"greedy_search";c.max_active_paths=options.paths;c.blank_penalty=options.blank_penalty;
        recognizer_=create_(&c);
        if(!recognizer_){close();return false;}
        return true;
    }
    bool begin(){reset();if(!recognizer_)return false;stream_=stream_create_(recognizer_);return stream_!=nullptr;}
    bool feed(const float* pcm,size_t n,std::string& out,bool(*abort)(void*),void* opaque){
        if(!stream_||abort(opaque)||n>16000)return false;
        feed_(stream_,16000,pcm,int32_t(n));return read(out,abort,opaque);
    }
    bool finish(std::string& out,bool(*abort)(void*),void* opaque){
        if(!stream_||abort(opaque))return false;
        finish_(stream_);return read(out,abort,opaque);
    }
};
}
