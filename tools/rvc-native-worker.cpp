// Private resident native RVC worker. Inherited AF_UNIX stream on stdin;
// no network listener, no fallback, no model load in OBS.
#include <audiocpp.h>
#include "framework/rvc_native_protocol.hpp"
#include <openssl/evp.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <vector>
#include <memory>
#include <fstream>
#include <string>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <cerrno>
#include <cstring>
namespace {
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
void verify(const char* path){
    std::ifstream f(path,std::ios::binary);check(bool(f),"RVC model missing");
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    check(ctx&&EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)==1,"SHA init");
    std::array<char,65536> buf{};size_t bytes=0;
    while(f){f.read(buf.data(),buf.size());auto n=f.gcount();if(n>0){bytes+=size_t(n);check(bytes<=1260187648&&EVP_DigestUpdate(ctx.get(),buf.data(),size_t(n))==1,"RVC model size/checksum");}}
    unsigned char hash[32];unsigned n=0;
    check(!f.bad()&&bytes==1260187648&&EVP_DigestFinal_ex(ctx.get(),hash,&n)==1&&n==32,"RVC model hash");
    char hex[65]{};for(size_t i=0;i<32;++i)std::snprintf(hex+2*i,3,"%02x",hash[i]);
    check(std::string(hex)=="09bd8f233e7ae82697826513c94d754c3904e58e30ad503b83437defccf99a72","Unqualified RVC GGUF");
}
// Runtime query respects CUDA_VISIBLE_DEVICES, unlike an NVML physical index.
struct CudaBudget{
    void* lib=nullptr;
    int (*memory)(size_t*,size_t*)=nullptr;
    explicit CudaBudget(bool enabled){
        if(!enabled)return;
        lib=dlopen("libcudart.so.12",RTLD_NOW|RTLD_LOCAL);
        if(!lib)throw std::runtime_error("CUDA runtime unavailable");
        try{
            auto select=reinterpret_cast<int(*)(int)>(dlsym(lib,"cudaSetDevice"));
            memory=reinterpret_cast<int(*)(size_t*,size_t*)>(dlsym(lib,"cudaMemGetInfo"));
            check(select&&memory&&select(0)==0,"CUDA device 0 unavailable");
            reserve(4000);
        }catch(...){dlclose(lib);throw;}
    }
    ~CudaBudget(){if(lib)dlclose(lib);}
    void reserve(size_t mib){
        if(!memory)return;
        size_t free=0,total=0;
        check(memory(&free,&total)==0&&free>=mib*1024*1024,"Insufficient CUDA memory headroom");
    }
};
#define API(X) X(abi_version) X(last_error) X(registry_create) X(registry_free) X(model_load) X(model_free) X(options_create) X(options_set) X(options_free) X(session_create) X(session_free) X(request_create) X(request_free) X(request_set_audio) X(request_set_option) X(session_run) X(result_audio) X(result_free)
struct Codec {
    void* lib=nullptr;
#define MEMBER(n) decltype(&audiocpp_##n) n=nullptr;
    API(MEMBER)
#undef MEMBER
    audiocpp_registry* registry=nullptr;
    audiocpp_model* model=nullptr;
    audiocpp_session* session=nullptr;
    void status(audiocpp_status s){if(s!=0)throw std::runtime_error(last_error()?last_error():"RVC ABI failed");}
    Codec(const char* library,const char* model_path,const char* device){
        verify(model_path);
        lib=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!lib)throw std::runtime_error(dlerror());
        try{
#define LOAD(n) n=reinterpret_cast<decltype(n)>(dlsym(lib,"audiocpp_" #n));check(n!=nullptr,"Missing ABI " #n);
            API(LOAD)
#undef LOAD
            check((abi_version()>>8)==2,"RVC ABI version");
            status(registry_create(nullptr,&registry));
            audiocpp_model_config config{};config.family_hint="rvc";
            status(model_load(registry,model_path,&config,nullptr,&model));
            std::unique_ptr<audiocpp_options,decltype(options_free)> options(options_create(),options_free);
            check(bool(options),"RVC options");
            status(options_set(options.get(),"rvc.weight_type","f32"));
            status(options_set(options.get(),"rvc.voice_cache_slots","1"));
            audiocpp_backend_config backend{device,0,6};
            status(session_create(model,"vc","offline",&backend,options.get(),&session));
        }catch(...){close();throw;}
    }
    ~Codec(){close();}
    void close(){if(session)session_free(session);if(model)model_free(model);if(registry)registry_free(registry);if(lib)dlclose(lib);}
    std::vector<float> convert(const float* input,uint32_t rate,int pitch){
        std::unique_ptr<audiocpp_request,decltype(request_free)> req(request_create(),request_free);
        check(bool(req),"RVC request");
        status(request_set_audio(req.get(),input,rvc_native::input_frames,16000,1));
        auto set=[&](const char* k,const std::string& v){status(request_set_option(req.get(),k,v.c_str()));};
        set("voice_id","default");set("retrieval_blend","0");set("semitone_shift",std::to_string(pitch));set("output_sample_rate",std::to_string(rate));
        audiocpp_result* raw=nullptr;auto rc=session_run(session,req.get(),&raw);
        std::unique_ptr<audiocpp_result,decltype(result_free)> result(raw,result_free);status(rc);
        const float* samples=nullptr;size_t frames=0;int hz=0,ch=0;
        status(result_audio(result.get(),&samples,&frames,&hz,&ch));
        check(samples&&hz==int(rate)&&ch==1&&frames>=rate*88/100&&frames<=rate,"Unexpected RVC geometry");
        std::vector<float> out(samples,samples+frames);
        for(float& v:out){check(std::isfinite(v),"RVC nonfinite output");v=std::clamp(v,-1.f,1.f);}
        return out;
    }
};
#undef API
bool transfer(void* data,size_t bytes,bool write){
    auto* p=static_cast<char*>(data);
    while(bytes){ssize_t n=write?send(0,p,bytes,MSG_NOSIGNAL):recv(0,p,bytes,0);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;bytes-=size_t(n);}
    return true;
}
}
int main(int argc,char** argv){
    using namespace rvc_native;
    if(argc!=4|| (std::strcmp(argv[3],"cuda")&&std::strcmp(argv[3],"cpu")))return 2;
    try{
        int type=0;socklen_t n=sizeof(type);check(getsockopt(0,SOL_SOCKET,SO_TYPE,&type,&n)==0&&type==SOCK_STREAM,"Expected private socket");
        CudaBudget budget(std::strcmp(argv[3],"cuda")==0);
        Codec codec(argv[1],argv[2],argv[3]);
        std::array<float,input_frames> pcm{};
        for(;;){
            Request req{};if(!transfer(&req,sizeof(req),false))return 0;
            if(req.magic!=magic||req.frames!=input_frames)return 3;
            if(!transfer(pcm.data(),sizeof(pcm),false))return 4;
            Reply reply{magic,req.sequence,1,0};
            if(!valid(req)||!std::all_of(pcm.begin(),pcm.end(),[](float v){return std::isfinite(v)&&std::abs(v)<=1;})){
                if(!transfer(&reply,sizeof(reply),true))return 5;
                continue;
            }
            budget.reserve(1024);
            auto out=codec.convert(pcm.data(),req.rate,req.semitones);
            budget.reserve(1024);
            reply.status=0;reply.frames=uint32_t(out.size());
            if(!transfer(&reply,sizeof(reply),true)||!transfer(out.data(),out.size()*sizeof(float),true))return 6;
        }
    }catch(const std::exception& e){std::fprintf(stderr,"RVC worker: %s\n",e.what());return 1;}
}
