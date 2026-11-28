// Optional native CUDA numerical comparison, not live OBS acceptance.
// Input and reference are three matching interleaved 2.8 s stereo 44.1 kHz float32 contexts.
#include "audiocpp.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>
#include <stdexcept>

static void check(audiocpp_status s){
 if(s!=AUDIOCPP_OK)throw std::runtime_error(audiocpp_last_error());
}
template<class T,void(*Free)(T*)> using Handle=std::unique_ptr<T,decltype(Free)>;
static std::vector<float> read(const char* path,size_t n){
 std::ifstream in(path,std::ios::binary);
 std::vector<float> x(n);
 if(!in.read(reinterpret_cast<char*>(x.data()),x.size()*4)||in.peek()!=EOF)
   throw std::runtime_error("Incorrect fixture size");
 return x;
}
int main(int argc,char**argv){
 try{
 if(argc!=5)throw std::runtime_error("model input.raw reference.raw weight_type");
 constexpr size_t frames=123480, samples=frames*2, windows=3;
 auto x=read(argv[2],samples*windows),ref=read(argv[3],samples*windows);
 audiocpp_registry* r=nullptr;check(audiocpp_registry_create(nullptr,&r));
 Handle<audiocpp_registry,audiocpp_registry_free> registry(r,audiocpp_registry_free);
 audiocpp_model_config conf{};conf.family_hint="mel_band_roformer";
 audiocpp_model* m=nullptr;
 auto start=std::chrono::steady_clock::now();
 check(audiocpp_model_load(r,argv[1],&conf,nullptr,&m));
 Handle<audiocpp_model,audiocpp_model_free> model(m,audiocpp_model_free);
 auto* o=audiocpp_options_create();
 Handle<audiocpp_options,audiocpp_options_free> opts(o,audiocpp_options_free);
 check(audiocpp_options_set(o,"mel_band_roformer.weight_type",argv[4]));
 check(audiocpp_options_set(o,"mel_band_roformer.num_overlap","1"));
 audiocpp_backend_config backend{"cuda",0,4};
 audiocpp_session* s=nullptr;check(audiocpp_session_create(m,"sep","offline",&backend,o,&s));
 Handle<audiocpp_session,audiocpp_session_free> session(s,audiocpp_session_free);
 printf("NATIVE_SESSION family=%s weight=%s startup_s=%.4f\n",audiocpp_session_family(s),argv[4],std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());fflush(stdout);
 for(size_t i=0;i<=windows;++i){
   auto idx=i?i-1:0;
   auto* q=audiocpp_request_create();
   Handle<audiocpp_request,audiocpp_request_free> request(q,audiocpp_request_free);
   check(audiocpp_request_set_audio(q,x.data()+idx*samples,frames,44100,2));
   audiocpp_result* y=nullptr;
   start=std::chrono::steady_clock::now();
   check(audiocpp_session_run(s,q,&y));
   double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
   Handle<audiocpp_result,audiocpp_result_free> result(y,audiocpp_result_free);
   if(audiocpp_result_named_audio_count(y)!=2)throw std::runtime_error("Expected two actual named outputs");
   const float *v=nullptr,*ins=nullptr;
   for(size_t k=0;k<2;++k){
     const char* id=nullptr;const float* data=nullptr;size_t n=0;int rate=0,channels=0;
     check(audiocpp_result_named_audio(y,k,&id,&data,&n,&rate,&channels));
     if(n!=frames||rate!=44100||channels!=2||!data)throw std::runtime_error("Unexpected output geometry");
     if(std::string(id)=="vocals")v=data;
     else if(std::string(id)=="instrumental")ins=data;
     else throw std::runtime_error("Unexpected stem name");
   }
   if(!v||!ins)throw std::runtime_error("Missing stem");
   double sq=0,den=0,energy=0,peak=0,recon=0;
   for(size_t k=0;k<samples;++k){
     double a=v[k],b=ins[k],expected=ref[idx*samples+k];
     if(!std::isfinite(a)||!std::isfinite(b))throw std::runtime_error("Nonfinite native output");
     sq+=(a-expected)*(a-expected);den+=expected*expected;energy+=a*a;
     peak=std::max(peak,std::abs(a-expected));recon=std::max(recon,std::abs(a+b-x[idx*samples+k]));
   }
   if(energy<=0||den<=0||recon>2e-6)throw std::runtime_error("Invalid output or reconstruction");
   printf("NATIVE_WINDOW warmup=%d context=%zu time_s=%.4f ref_rel_rms=%.6f ref_max=%.6f recon_max=%.9g\n",i==0,idx,elapsed,std::sqrt(sq/den),peak,recon);fflush(stdout);
 }
 return 0;
 }catch(const std::exception& e){fprintf(stderr,"NATIVE_PROBE_ERROR: %s\n",e.what());return 1;}
}
