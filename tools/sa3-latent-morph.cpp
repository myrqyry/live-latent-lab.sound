// Native SAME-S latent morphing, using the pinned sa3.cpp headers and
// its existing private GGML runtime. Separate process; no OBS ABI changes.
#include "gguf_model.h"
#include "same_ae.h"
#include "wav.h"
#include "../src/framework/same_texture.hpp"
#include "../src/framework/same_resident.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <memory>
#include <array>
#include <openssl/evp.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <vector>
#include <sys/socket.h>
#include <cerrno>
#include <cstdint>
#include <limits>

namespace {
using Clock=std::chrono::steady_clock;
long long ms(Clock::time_point t){
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t).count();
}
constexpr int sample_rate=44100,max_samples=12*sample_rate;
void verify_model(const char* path){
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)!=216721728)
        throw std::runtime_error("Missing or unqualified SAME-S model");
    std::ifstream f(path,std::ios::binary);
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if(!f||!ctx||EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1)throw std::runtime_error("Model verification failed");
    std::array<char,65536> buf{};
    while(f.read(buf.data(),buf.size())||f.gcount())
        if(EVP_DigestUpdate(ctx.get(),buf.data(),size_t(f.gcount()))!=1)throw std::runtime_error("Model verification failed");
    unsigned char digest[EVP_MAX_MD_SIZE];unsigned len=0;
    if(!f.eof()||f.bad()||EVP_DigestFinal_ex(ctx.get(),digest,&len)!=1||len!=32)throw std::runtime_error("Model verification failed");
    static constexpr char hex[]="0123456789abcdef";
    std::string got;for(unsigned i=0;i<len;++i){got+=hex[digest[i]>>4];got+=hex[digest[i]&15];}
    if(got!="ae34819e33cb188e54c4102125e0926d6739978772923b05e00ce9e74684e736")
        throw std::runtime_error("Unqualified SAME-S model checksum");
}
void write_output(const char* path,const std::vector<float>& audio,int frames,int stride){
    const int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    if(fd<0)throw std::runtime_error("Cannot create output WAV exclusively");
    FILE* f=fdopen(fd,"wb");
    if(!f){close(fd);unlink(path);throw std::runtime_error("Cannot open output WAV");}
    auto data=sa3::wav_detail::interleave_planar_i16(audio.data(),frames,2,stride);
    uint32_t bytes=uint32_t(data.size()*2),riff=36+bytes,fmt=16,rate=44100,byte_rate=rate*4;
    uint16_t pcm=1,ch=2,align=4,bits=16;
    auto put=[&](const void* p,size_t n){return fwrite(p,1,n,f)==n;};
    bool ok=put("RIFF",4)&&put(&riff,4)&&put("WAVEfmt ",8)&&put(&fmt,4)&&put(&pcm,2)&&
        put(&ch,2)&&put(&rate,4)&&put(&byte_rate,4)&&put(&align,2)&&put(&bits,2)&&put("data",4)&&put(&bytes,4)&&put(data.data(),bytes);
    if(fclose(f)!=0)ok=false;
    if(!ok){unlink(path);throw std::runtime_error("Output WAV write failed");}
}
struct Audio {int frames=0;std::vector<float> pcm;};
Audio read(const std::string& path){
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>max_samples*8+65536)
        throw std::runtime_error("Input must be a bounded WAV file");
    Audio a;int ch=0,sr=0;
    a.pcm=sa3::read_wav_planar(path,a.frames,ch,sr);
    if(sr!=sample_rate||ch!=2||a.frames<8192||a.frames>max_samples||
       a.pcm.size()!=size_t(a.frames)*2)throw std::runtime_error("Use 44.1 kHz stereo WAV, 8192 samples to 12 seconds");
    for(float v:a.pcm)if(!std::isfinite(v))throw std::runtime_error("Nonfinite input");
    return a;
}
struct Graph {
    ggml_context* ctx=nullptr;
    ggml_gallocr_t alloc=nullptr;
    ggml_cgraph* graph=nullptr;
    ggml_tensor *in=nullptr,*out=nullptr,*pos=nullptr,*pos2=nullptr;
    sa3::GgufModel& model;
    const int n,n2;
    std::vector<int32_t> positions;
    Graph(sa3::GgufModel& w,const sa3::SameConfig& c,int frames,bool encode):
        model(w),n(frames*c.sub_chunk),n2(n+2*c.shift),positions(n2){
        for(int j=0;j<n2;++j)positions[j]=j;
        try{
            ctx=ggml_init({64*1024*1024,nullptr,true});
            if(!ctx)throw std::runtime_error("Graph context allocation failed");
            in=encode?ggml_new_tensor_2d(ctx,GGML_TYPE_F32,frames*c.patch_size*c.output_seg,2):
                      ggml_new_tensor_2d(ctx,GGML_TYPE_F32,c.latent,frames);
            pos=ggml_new_tensor_1d(ctx,GGML_TYPE_I32,n);
            pos2=ggml_new_tensor_1d(ctx,GGML_TYPE_I32,n2);
            auto* mask=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,n,n);
            auto* mask2=ggml_new_tensor_2d(ctx,GGML_TYPE_F32,n2,n2);
            for(auto* t:{in,pos,pos2,mask,mask2})ggml_set_input(t);
            out=ggml_cont(ctx,encode?sa3::same_encode(ctx,w,in,c,frames,pos,mask,pos2,mask2).z:
                                    sa3::same_decode(ctx,w,in,c,frames,pos,mask,pos2,mask2).audio);
            ggml_set_output(out);graph=ggml_new_graph_custom(ctx,32768,false);
            ggml_build_forward_expand(graph,out);
            alloc=ggml_gallocr_new(ggml_backend_get_default_buffer_type(w.backend));
            if(!alloc||!ggml_gallocr_alloc_graph(alloc,graph))throw std::runtime_error("Graph buffer allocation failed");
        }catch(...){release();throw;}
    }
    Graph(const Graph&)=delete;
    Graph& operator=(const Graph&)=delete;
    void release(){if(alloc)ggml_gallocr_free(alloc);if(ctx)ggml_free(ctx);alloc=nullptr;ctx=nullptr;}
    ~Graph(){release();}
    void run_into(const std::vector<float>& input,std::vector<float>& result){
        if(input.size()!=size_t(ggml_nelements(in)))throw std::runtime_error("Wrong input shape");
        ggml_backend_tensor_set(in,input.data(),0,input.size()*sizeof(float));
        for(auto* t:{pos,pos2}){
            ggml_backend_tensor_set(t,positions.data(),0,size_t(ggml_nelements(t))*sizeof(int32_t));
        }
        std::string err;
        if(!sa3::graph_compute_checked(model.backend,graph,"SAME graph",err))throw std::runtime_error(err);
        result.resize(ggml_nelements(out));
        ggml_backend_tensor_get(out,result.data(),0,result.size()*sizeof(float));
        for(float v:result)if(!std::isfinite(v))throw std::runtime_error("Nonfinite model output");
    }
    std::vector<float> run(const std::vector<float>& input){std::vector<float> out;run_into(input,out);return out;}
};
std::vector<float> pad(const Audio& a,int n){
    std::vector<float> out(size_t(n)*2);
    for(int c=0;c<2;++c)std::copy_n(a.pcm.data()+c*a.frames,a.frames,out.data()+c*n);
    return out;
}
// Private local socket transport. No listener, path-based commands or
// network service: the caller passes an already connected socket as stdin.
// Requests and replies are fixed little-endian headers followed by planar f32.
using namespace same_resident;
bool receive(void* p,size_t n,bool allow_eof=false){
    auto* b=static_cast<char*>(p);size_t done=0;
    while(done<n){
        auto r=recv(STDIN_FILENO,b+done,n-done,0);
        if(r<0&&errno==EINTR)continue;
        if(r==0&&done==0&&allow_eof)return false;
        if(r<=0)throw std::runtime_error("Incomplete resident request");
        done+=size_t(r);
    }
    return true;
}
void send_all(const void* p,size_t n){
    auto* b=static_cast<const char*>(p);
    while(n){
        auto r=send(STDIN_FILENO,b,n,MSG_NOSIGNAL);
        if(r<0&&errno==EINTR)continue;
        if(r<=0)throw std::runtime_error("Resident response failed");
        b+=r;n-=size_t(r);
    }
}
struct Resident {
    std::unique_ptr<ggml_backend,decltype(&ggml_backend_free)> backend;
    sa3::GgufModel weights;
    sa3::SameConfig config;
    std::unique_ptr<Graph> enc,dec;
    int shape=0;uint32_t builds=0;
    std::vector<float> input,padded,latent,valid,modified,decoded,result;
    static ggml_backend_t select(const char* device){
        const std::string d=device;
        if(d!="auto"&&d!="cpu"&&d!="gpu:CUDA0")throw std::runtime_error("Unsupported device");
        auto b=sa3::make_backend(8,device);
        if(!b)throw std::runtime_error("Backend unavailable");
        if((d=="gpu:CUDA0"&&std::string(ggml_backend_name(b))!="CUDA0")||
           (d=="cpu"&&ggml_backend_dev_type(ggml_backend_get_device(b))!=GGML_BACKEND_DEVICE_TYPE_CPU)){
            ggml_backend_free(b);throw std::runtime_error("Requested backend unavailable");
        }
        return b;
    }
    Resident(const char* model,const char* device):
        backend(select(device),ggml_backend_free),weights(sa3::load_gguf(model,backend.get())),config(sa3::SameConfig::from(weights)){
        if(!config.chunk||config.patch_size*config.output_seg!=4096||config.out_channels/config.patch_size!=2||config.latent!=256)
            throw std::runtime_error("Unsupported SAME-S geometry");
    }
    bool valid_request(const Request& r){
        return r.mode>=1&&r.mode<=3&&std::isfinite(r.amount)&&r.amount>=0&&r.amount<=1&&
            std::isfinite(r.position)&&r.position>=0&&r.position<=1&&r.radius>=1&&r.radius<=16&&r.grain>=1&&r.grain<=16&&
            std::all_of(input.begin(),input.end(),[](float v){return std::isfinite(v)&&std::abs(v)<=1;});
    }
    void process(const Request& r){
        int frames=int((r.frames+8191)/8192*2),n=frames*4096;
        if(shape!=frames){
            // Release both old graphs before allocating replacement geometry.
            enc.reset();dec.reset();shape=0;
            enc=std::make_unique<Graph>(weights,config,frames,true);
            dec=std::make_unique<Graph>(weights,config,frames,false);
            shape=frames;++builds;
        }
        padded.assign(size_t(n)*2,0);
        for(int ch=0;ch<2;++ch)std::copy_n(input.data()+ch*r.frames,r.frames,padded.data()+ch*n);
        enc->run_into(padded,latent);
        const int count=int((r.frames+4095)/4096);
        valid.assign(latent.begin(),latent.begin()+size_t(count)*config.latent);
        auto mode=r.mode==1?same_texture::Mode::Freeze:r.mode==2?same_texture::Mode::Smear:same_texture::Mode::Grain;
        same_texture::process_into(valid,modified,config.latent,count,mode,r.amount,r.position,int(r.radius),int(r.grain));
        std::copy(modified.begin(),modified.end(),latent.begin());
        dec->run_into(latent,decoded);
        if(decoded.size()!=size_t(n)*2)throw std::runtime_error("Invalid decoded shape");
        float peak=0;for(float v:decoded)peak=std::max(peak,std::abs(v));
        const float gain=peak>.75f?.75f/peak:1.f;
        result.resize(size_t(r.frames)*2);
        for(int ch=0;ch<2;++ch)for(uint32_t j=0;j<r.frames;++j)result[size_t(ch)*r.frames+j]=decoded[size_t(ch)*n+j]*gain;
    }
};
int serve(const char* model,const char* device){
    verify_model(model);
    Resident codec(model,device);
    std::cerr<<"SAME_RESIDENT_READY device="<<ggml_backend_name(codec.backend.get())<<std::endl;
    Request r{};
    while(receive(&r,sizeof r,true)){
        // Bad framing is fatal; do not allocate or drain an unbounded payload.
        if(r.magic!=magic||r.frames<8192||r.frames>max_samples||!r.sequence)
            throw std::runtime_error("Invalid resident framing");
        codec.input.resize(size_t(r.frames)*2);
        receive(codec.input.data(),codec.input.size()*sizeof(float));
        Reply reply{magic,r.sequence,1,0,codec.builds,0};
        if(codec.valid_request(r)){
            const auto t=Clock::now();
            // Backend failures terminate the helper: never continue on a
            // potentially poisoned graph or silently return stale audio.
            codec.process(r);
            reply={magic,r.sequence,0,r.frames,codec.builds,uint32_t(ms(t))};
        }
        send_all(&reply,sizeof reply);
        if(reply.status==0)send_all(codec.result.data(),codec.result.size()*sizeof(float));
    }
    return 0;
}

}
int main(int argc,char** argv){
    try{
        if(argc==4&&std::string(argv[1])=="--serve")return serve(argv[2],argv[3]);
        if(argc!=8&&argc!=11)throw std::runtime_error("usage: sa3-latent-morph MODEL A.wav B.wav MIX OUTPUT DEVICE fixed|ramp; or MODE freeze|smear|grain POSITION RADIUS GRAIN (B ignored)");
        const std::string mix_text=argv[4];size_t parsed=0;
        const float mix=std::stof(mix_text,&parsed);
        if(parsed!=mix_text.size()||!std::isfinite(mix)||mix<0||mix>1)throw std::runtime_error("Mix must be 0..1");
        const std::string device=argv[6],mode=argv[7];
        const bool blend=mode=="fixed"||mode=="ramp";
        if((device!="auto"&&device!="cpu"&&device!="gpu:CUDA0")||
           (!blend&&mode!="freeze"&&mode!="smear"&&mode!="grain")||
           (blend?argc!=8:argc!=11))throw std::runtime_error("Unsupported device or mode");
        float position=.5f;int radius=2,grain=2;
        if(!blend){
            auto number=[](const char* raw){
                std::string str=raw;size_t n=0;float v=std::stof(str,&n);
                if(n!=str.size()||!std::isfinite(v))throw std::runtime_error("Invalid texture parameter");
                return v;
            };
            position=number(argv[8]);float r=number(argv[9]),g=number(argv[10]);
            if(position<0||position>1||r<1||r>16||g<1||g>16||std::floor(r)!=r||std::floor(g)!=g)
                throw std::runtime_error("Invalid texture parameter");
            radius=int(r);grain=int(g);
        }
        if(std::filesystem::exists(argv[5]))throw std::runtime_error("Output already exists");
        auto a=read(argv[2]);Audio b;if(blend)b=read(argv[3]);
        if(blend&&a.frames!=b.frames)throw std::runtime_error("Input WAV durations must match");
        auto t=Clock::now();verify_model(argv[1]);std::cout<<"MODEL_VERIFY_MS="<<ms(t)<<std::endl;
        t=Clock::now();
        auto backend=std::unique_ptr<ggml_backend,decltype(&ggml_backend_free)>(sa3::make_backend(8,device.c_str()),ggml_backend_free);
        if(!backend)throw std::runtime_error("Backend unavailable");
        auto w=sa3::load_gguf(argv[1],backend.get());
        if(device=="cpu"&&ggml_backend_dev_type(ggml_backend_get_device(w.backend))!=GGML_BACKEND_DEVICE_TYPE_CPU)throw std::runtime_error("Unexpected GPU");
        if(device=="gpu:CUDA0"&&std::string(ggml_backend_name(w.backend))!="CUDA0")throw std::runtime_error("CUDA unavailable; no silent fallback");
        auto c=sa3::SameConfig::from(w);
        if(!c.chunk||c.patch_size*c.output_seg!=4096||c.out_channels/c.patch_size!=2||c.latent<=0||c.latent>256)
            throw std::runtime_error("Only qualified SAME-S geometry supported");
        int frames=(a.frames+8191)/8192*2,n=frames*4096;
        std::cout<<"MODEL_LOAD_MS="<<ms(t)<<" DEVICE="<<ggml_backend_name(w.backend)<<" LATENT_DIM="<<c.latent<<" FRAMES="<<frames<<std::endl;
        std::vector<float> za,zb;
        t=Clock::now();
        {Graph enc(w,c,frames,true);za=enc.run(pad(a,n));if(blend)zb=enc.run(pad(b,n));}
        std::cout<<"ENCODE_BOTH_MS="<<ms(t)<<std::endl;
        if(za.size()!=size_t(c.latent)*frames||(blend&&zb.size()!=za.size()))throw std::runtime_error("Unexpected latent shape");
        if(blend){
            double delta=0;
            for(size_t i=0;i<za.size();++i){
                delta+=std::abs(za[i]-zb[i]);
                const float weight=mode=="ramp"?mix*float(i/c.latent)/float(frames-1):mix;
                za[i]=(1-weight)*za[i]+weight*zb[i];
            }
            std::cout<<"LATENT_MEAN_ABS_DELTA="<<delta/za.size()<<std::endl;
        }else{
            // Exclude wholly padded frames from selecting anchors or grains.
            const int valid_frames=(a.frames+4095)/4096;
            const size_t valid_size=size_t(c.latent)*valid_frames;
            std::vector<float> valid(za.begin(),za.begin()+valid_size);
            const auto effect=mode=="freeze"?same_texture::Mode::Freeze:mode=="smear"?same_texture::Mode::Smear:same_texture::Mode::Grain;
            auto texture=same_texture::process(valid,c.latent,valid_frames,effect,mix,position,radius,grain);
            double delta=0;for(size_t i=0;i<valid_size;++i)delta+=std::abs(valid[i]-texture[i]);
            std::copy(texture.begin(),texture.end(),za.begin());
            // The final padding latent remains the original, not repeated content.
            std::cout<<"TEXTURE_LATENT_DELTA="<<delta/valid_size<<std::endl;
        }
        t=Clock::now();std::vector<float> audio;
        {Graph dec(w,c,frames,false);audio=dec.run(za);}
        std::cout<<"DECODE_MS="<<ms(t)<<std::endl;
        if(audio.size()!=size_t(n)*2)throw std::runtime_error("Unexpected decoder shape");
        // Keep output length exact, preserve headroom without hard clipping.
        float peak=0;for(float v:audio)peak=std::max(peak,std::abs(v));
        if(peak>.75f)for(float& v:audio)v*=.75f/peak;
        write_output(argv[5],audio,a.frames,n);
        std::cout<<"MORPH_OK=1 SAMPLES="<<a.frames<<" MIX="<<mix<<" MODE="<<mode<<std::endl;
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
