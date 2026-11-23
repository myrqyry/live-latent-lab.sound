#include <transcribe.h>
#include <transcribe/parakeet.h>
#include <cstring>
#include <string>
#include <new>
#include <memory>
#include <cstdint>

struct Session {
 transcribe_session* session=nullptr;
 std::string language;
 std::string device_backend;
 int right=3;
 ~Session(){if(session)transcribe_close(session);}
};
#define API extern "C" __attribute__((visibility("default")))
API int sound_nemotron_abi(){return 1;}

// Keep the original ABI working for callers with no device preference.
// AUTO selects the fastest available registered GPU before CPU.
API void* sound_nemotron_open_v2(const char* path,int threads,const char* lang,int right,const char* device){
 if(!path||!lang||!device||threads<1||(right!=0&&right!=3&&right!=6&&right!=13))return nullptr;
 transcribe_backend_request backend;
 if(std::strcmp(device,"auto")==0)backend=TRANSCRIBE_BACKEND_AUTO;
 else if(std::strcmp(device,"cpu")==0)backend=TRANSCRIBE_BACKEND_CPU;
 else if(std::strcmp(device,"cuda")==0)backend=TRANSCRIBE_BACKEND_CUDA;
 else return nullptr;
 try{
  auto s=std::make_unique<Session>();
  transcribe_model_load_params load{};transcribe_model_load_params_init(&load);load.backend=backend;
  transcribe_session_params sp{};transcribe_session_params_init(&sp);sp.n_threads=threads;
  if(transcribe_open(path,&load,&sp,&s->session)!=TRANSCRIBE_OK||!s->session){
   // If AUTO could select a GPU but model construction failed, retry on CPU.
   // Explicit CUDA never silently substitutes CPU.
   if(s->session){transcribe_close(s->session);s->session=nullptr;}
   if(backend!=TRANSCRIBE_BACKEND_AUTO)return nullptr;
   load.backend=TRANSCRIBE_BACKEND_CPU;
   if(transcribe_open(path,&load,&sp,&s->session)!=TRANSCRIBE_OK||!s->session)return nullptr;
  }
  const auto* model=transcribe_get_model(s->session);
  if(!transcribe_model_accepts_ext_kind(model,TRANSCRIBE_EXT_SLOT_STREAM,TRANSCRIBE_EXT_KIND_PARAKEET_STREAM))return nullptr;
  const char* actual=transcribe_model_backend(model);
  s->device_backend=actual?actual:"unknown";
  s->language=lang;s->right=right;return s.release();
 }catch(...){return nullptr;}
}
API void* sound_nemotron_open(const char* path,int threads,const char* lang,int right){
 return sound_nemotron_open_v2(path,threads,lang,right,"auto");
}
API const char* sound_nemotron_backend(void* p){
 return p?static_cast<Session*>(p)->device_backend.c_str():"unknown";
}
API void sound_nemotron_close(void* p){delete static_cast<Session*>(p);}
API int sound_nemotron_begin(void* p){
 auto* s=static_cast<Session*>(p);if(!s)return 0;
 transcribe_stream_reset(s->session);
 transcribe_run_params run{};transcribe_run_params_init(&run);run.language=(s->language=="auto"||s->language.empty())?nullptr:s->language.c_str();run.timestamps=TRANSCRIBE_TIMESTAMPS_NONE;
 transcribe_stream_params stream{};transcribe_stream_params_init(&stream);
 transcribe_parakeet_stream_ext ext{};transcribe_parakeet_stream_ext_init(&ext);ext.att_context_right=s->right;stream.family=&ext.ext;
 return transcribe_stream_begin(s->session,&run,&stream)==TRANSCRIBE_OK;
}
API void sound_nemotron_reset(void* p){if(p)transcribe_stream_reset(static_cast<Session*>(p)->session);}
API int sound_nemotron_feed(void* p,const float* pcm,int count){
 auto* s=static_cast<Session*>(p);return s&&count>0&&count<=17920&&transcribe_stream_feed(s->session,pcm,count,nullptr)==TRANSCRIBE_OK;
}
API int sound_nemotron_finish(void* p){auto* s=static_cast<Session*>(p);return s&&transcribe_stream_finalize(s->session,nullptr)==TRANSCRIBE_OK;}
API int sound_nemotron_text(void* p,char* buffer,size_t capacity){
 auto* s=static_cast<Session*>(p);if(!s||!buffer||!capacity)return 0;
 transcribe_stream_text view{};transcribe_stream_text_init(&view);
 if(transcribe_stream_get_text(s->session,&view)!=TRANSCRIBE_OK||view.full_text_bytes>=capacity)return 0;
 if(view.full_text_bytes&&!view.full_text)return 0;
 if(view.full_text_bytes)std::memcpy(buffer,view.full_text,view.full_text_bytes);
 buffer[view.full_text_bytes]=0;return 1;
}
