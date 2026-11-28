// Isolated native audio.cpp/CUDA separator. LLSTEM02 profile 1.
// All inference, resampling and socket IO are outside the OBS process.
#include "audiocpp.h"
#include "framework/live-stem-client.hpp"
#include <samplerate.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <sys/prctl.h>
#include <signal.h>

namespace {
using namespace stts;
constexpr size_t context=123480;
template<class T,void(*Free)(T*)>using Handle=std::unique_ptr<T,decltype(Free)>;
void check(audiocpp_status s){if(s!=AUDIOCPP_OK)throw std::runtime_error(audiocpp_last_error());}
bool finite(const std::vector<float>& v){
    return std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);});
}
std::vector<float> resample(const std::vector<float>& in,size_t frames){
    if(in.size()==frames*2)return in;
    std::vector<float> out((frames+32)*2);
    SRC_DATA data{};
    data.data_in=in.data();data.input_frames=long(in.size()/2);
    data.data_out=out.data();data.output_frames=long(frames+32);
    data.src_ratio=double(frames)/double(data.input_frames);data.end_of_input=1;
    const auto error=src_simple(&data,SRC_SINC_MEDIUM_QUALITY,2);
    if(error)throw std::runtime_error(src_strerror(error));
    // The rational 44100->48000 conversion can yield one trailing
    // sample from floating-point endpoint rounding. Keep the exact host
    // frame interval; never pad a short conversion or move the cursor.
    if(data.input_frames_used!=data.input_frames||data.output_frames_gen<long(frames)||data.output_frames_gen>long(frames+1))
        throw std::runtime_error("Invalid resampled context length");
    out.resize(frames*2);
    if(!finite(out))throw std::runtime_error("Nonfinite resampled context");
    return out;
}
class Separator{
    Handle<audiocpp_registry,audiocpp_registry_free> registry_{nullptr,audiocpp_registry_free};
    Handle<audiocpp_model,audiocpp_model_free> model_{nullptr,audiocpp_model_free};
    Handle<audiocpp_session,audiocpp_session_free> session_{nullptr,audiocpp_session_free};
public:
    explicit Separator(const std::string& path){
        audiocpp_registry* r=nullptr;check(audiocpp_registry_create(nullptr,&r));registry_.reset(r);
        audiocpp_model_config conf{};conf.family_hint="mel_band_roformer";
        audiocpp_model* m=nullptr;check(audiocpp_model_load(r,path.c_str(),&conf,nullptr,&m));model_.reset(m);
        Handle<audiocpp_options,audiocpp_options_free> opts(audiocpp_options_create(),audiocpp_options_free);
        if(!opts)throw std::bad_alloc();
        check(audiocpp_options_set(opts.get(),"mel_band_roformer.weight_type","native"));
        check(audiocpp_options_set(opts.get(),"mel_band_roformer.num_overlap","1"));
        audiocpp_backend_config backend{"cuda",0,4};
        audiocpp_session* s=nullptr;check(audiocpp_session_create(m,"sep","offline",&backend,opts.get(),&s));session_.reset(s);
        if(std::string(audiocpp_session_family(s))!="mel_band_roformer")throw std::runtime_error("Wrong model family");
        separate(std::vector<float>(2*context),context); // warm the actual graph
    }
    std::vector<float> separate(const std::vector<float>& planar,size_t frames){
        if(planar.size()!=2*frames||!finite(planar))throw std::runtime_error("Invalid input");
        std::vector<float> mixture(2*frames);
        for(size_t i=0;i<frames;++i){mixture[2*i]=planar[i];mixture[2*i+1]=planar[frames+i];}
        auto model_pcm=resample(mixture,context);
        Handle<audiocpp_request,audiocpp_request_free> q(audiocpp_request_create(),audiocpp_request_free);
        if(!q)throw std::bad_alloc();
        check(audiocpp_request_set_audio(q.get(),model_pcm.data(),context,44100,2));
        audiocpp_result* y=nullptr;check(audiocpp_session_run(session_.get(),q.get(),&y));
        Handle<audiocpp_result,audiocpp_result_free> result(y,audiocpp_result_free);
        if(audiocpp_result_named_audio_count(y)!=2)throw std::runtime_error("Expected two native stems");
        const float *vocal=nullptr,*instrumental=nullptr;
        for(size_t i=0;i<2;++i){
            const char* id=nullptr;const float* data=nullptr;size_t n=0;int rate=0,ch=0;
            check(audiocpp_result_named_audio(y,i,&id,&data,&n,&rate,&ch));
            if(!id||!data||n!=context||rate!=44100||ch!=2)throw std::runtime_error("Invalid native stem geometry");
            if(std::string(id)=="vocals")vocal=data;
            else if(std::string(id)=="instrumental")instrumental=data;
            else throw std::runtime_error("Unexpected native stem");
        }
        if(!vocal||!instrumental)throw std::runtime_error("Missing native stems");
        for(size_t i=0;i<2*context;++i)
            if(!std::isfinite(vocal[i])||!std::isfinite(instrumental[i]))
                throw std::runtime_error("Nonfinite native stems");
        auto wet=resample(std::vector<float>(vocal,vocal+2*context),frames);
        std::vector<float> out(4*frames);
        for(size_t i=0;i<frames;++i){
            out[i]=wet[2*i];out[frames+i]=wet[2*i+1];
            out[2*frames+i]=planar[i]-out[i];out[3*frames+i]=planar[frames+i]-out[frames+i];
        }
        if(!finite(out))throw std::runtime_error("Nonfinite residual");
        return out;
    }
};
void serve(int fd,Separator& model){
    timeval limit{30,0};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit))||
       setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof(limit)))return;
    unsigned char h[40]{};
    if(!read_fully(fd,h,sizeof(h)))return;
    bool valid=std::memcmp(h,"LLSTEM02",8)==0&&get_le32(h+8)==2;
    auto rate=get_le32(h+12),frames=get_le32(h+16),seq=get_le32(h+24);
    valid=valid&&(rate==44100||rate==48000)&&frames==rate*28/10&&get_le32(h+20)==2&&
          !get_le32(h+28)&&!get_le32(h+32)&&get_le32(h+36)==1;
    unsigned char reply[40]{};
    std::memcpy(reply,"LLSTEM02",8);put_le32(reply+8,2);put_le32(reply+20,2);put_le32(reply+36,1);
    if(valid){put_le32(reply+12,rate);put_le32(reply+16,frames);put_le32(reply+24,seq);}
    try{
        if(!valid)throw std::runtime_error("Invalid v2 request");
        std::vector<float> pcm(2*frames);
        if(!read_fully(fd,pcm.data(),pcm.size()*sizeof(float)))throw std::runtime_error("Truncated request");
        auto start=std::chrono::steady_clock::now();
        auto out=model.separate(pcm,frames);
        printf("STEM_NATIVE_WINDOW seq=%u rate=%u processing=%.4f\n",seq,rate,
            std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());fflush(stdout);
        put_le32(reply+28,2);
        if(write_fully(fd,reply,sizeof reply))write_fully(fd,out.data(),out.size()*sizeof(float));
    }catch(const std::exception& e){
        fprintf(stderr,"STEM_NATIVE_REJECTED seq=%u %s\n",seq,e.what());
        put_le32(reply+28,0);put_le32(reply+32,1);write_fully(fd,reply,sizeof reply);
    }
}
}
int main(int argc,char**argv){
    try{
        std::string model,path;pid_t parent=0;
        for(int i=1;i<argc;i+=2){
            if(i+1>=argc)throw std::runtime_error("Missing argument");
            std::string key=argv[i],value=argv[i+1];
            if(key=="--model")model=value;
            else if(key=="--socket")path=value;
            else if(key=="--parent-pid")parent=pid_t(std::stol(value));
            else throw std::runtime_error("Unknown argument");
        }
        if(parent<=1||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)
            throw std::runtime_error("Owning process unavailable");
        if(std::filesystem::path(model).extension()!=".gguf"||!std::filesystem::is_regular_file(model))
            throw std::runtime_error("Select a local Mel-Band GGUF file");
        sockaddr_un address{};address.sun_family=AF_UNIX;
        auto dir=std::filesystem::path(path).parent_path();
        struct stat info{};
        if(path.empty()||path.size()>=sizeof(address.sun_path)||!std::filesystem::path(path).is_absolute()||
           lstat(dir.c_str(),&info)||!S_ISDIR(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&077))
            throw std::runtime_error("Expected private socket directory");
        if(lstat(path.c_str(),&info)==0||errno!=ENOENT)throw std::runtime_error("Socket already exists or inaccessible");
        umask(0077);std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        Separator separator(model);
        int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(server<0||bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))||chmod(path.c_str(),0600)||listen(server,1))
            throw std::runtime_error("Private endpoint setup failed");
        printf("STEM_NATIVE_READY family=mel_band_roformer backend=cuda profile=1\n");fflush(stdout);
        for(;;){
            int fd=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);
            if(fd<0){if(errno==EINTR)continue;throw std::runtime_error("Accept failed");}
            serve(fd,separator);close(fd);
        }
    }catch(const std::exception& e){fprintf(stderr,"STEM_NATIVE_ERROR: %s\n",e.what());return 1;}
}
