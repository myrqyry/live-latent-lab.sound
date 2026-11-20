#include <obs.h>
#include <obs-module.h>
#include <curl/curl.h>
#include <chrono>
#include <thread>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
using namespace std::chrono_literals;
static size_t receive(char* p,size_t n,size_t m,void* opaque){
    static_cast<std::string*>(opaque)->append(p,n*m);return n*m;
}
struct Model {int active=-1;bool loaded=false;};
static Model read_model(const char* name){
    auto* c=curl_easy_init();if(!c)return {};
    auto socket=std::string(getenv("XDG_RUNTIME_DIR"))+
        (std::string(name)=="chatterbox"?"/lll-sound-chatterbox/speech.sock":"/lll-sound-tts/speech.sock");
    std::string raw;
    curl_easy_setopt(c,CURLOPT_URL,"http://localhost/v1/models/status");
    curl_easy_setopt(c,CURLOPT_UNIX_SOCKET_PATH,socket.c_str());
    curl_easy_setopt(c,CURLOPT_PROXY,"");curl_easy_setopt(c,CURLOPT_TIMEOUT_MS,1500L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(c,CURLOPT_WRITEDATA,&raw);
    auto rc=curl_easy_perform(c);curl_easy_cleanup(c);
    if(rc!=CURLE_OK)return {};
    auto* d=obs_data_create_from_json(raw.c_str());if(!d)return {};
    auto* a=obs_data_get_obj(d,"active");Model out;
    if(a){out.active=int(obs_data_get_int(a,name));obs_data_release(a);}
    // OBS's obs_data cannot read arrays of primitive strings directly;
    // match the exact quoted model in the isolated loaded JSON array.
    auto first=raw.find("\"loaded\"");auto end=raw.find(']',first);
    out.loaded=first!=std::string::npos&&end!=std::string::npos&&
        raw.substr(first,end-first).find("\""+std::string(name)+"\"")!=std::string::npos;
    obs_data_release(d);return out;
}
static bool wait_model(const char* name,int active,bool loaded,int ms=8000){
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
    do{
        auto r=read_model(name);if(r.active==active&&r.loaded==loaded)return true;
        std::this_thread::sleep_for(100ms);
    }while(std::chrono::steady_clock::now()<end);
    return false;
}
static obs_source_t* make(const char* name,const char* backend,int mode,int ttl,int channel){
    auto* d=obs_data_create();obs_data_set_string(d,"backend",backend);
    obs_data_set_int(d,"retention_mode",mode);obs_data_set_int(d,"retention_ttl",ttl);
    obs_data_set_string(d,"text","Hello from Live Latent Lab.");
    auto* s=obs_source_create("sound_tts_source",name,d,nullptr);obs_data_release(d);
    if(s){obs_set_output_source(channel,s);obs_source_set_audio_active(s,true);}
    return s;
}
static void config(obs_source_t* s,const char* backend,int mode,int ttl=3){
    auto* d=obs_data_create();obs_data_set_string(d,"backend",backend);
    obs_data_set_int(d,"retention_mode",mode);obs_data_set_int(d,"retention_ttl",ttl);
    obs_source_update(s,d);obs_data_release(d);
}
static void speak(obs_source_t* s){
    auto* p=obs_source_properties(s);
    obs_property_button_clicked(obs_properties_get(p,"speak"),s);
    obs_properties_destroy(p);
}
static std::atomic<uint64_t> audible{0};
static void capture(void*,obs_source_t*,const audio_data* a,bool){
    auto* p=reinterpret_cast<const float*>(a->data[0]);if(!p)return;
    for(unsigned i=0;i<a->frames;++i)if(p[i]>.001f||p[i]<-.001f)++audible;
}
static bool speech(obs_source_t* s,int expected){
    const auto before=audible.load();speak(s);
    for(int i=0;i<700;++i){
        calldata_t a{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_tts_status_v1",&a);
        auto n=calldata_int(&a,"completed"),f=calldata_int(&a,"failed");calldata_free(&a);
        if(!ok||f)return false;
        if(n==expected)return audible.load()>before+1000;
        std::this_thread::sleep_for(50ms);
    }return false;
}
int main(int argc,char** argv){
    if(argc!=3||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;
    v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;
    v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_module_t* m=nullptr;if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 5;
    auto* a=make("Immediate","chatterbox",0,3,1);
    auto* b=make("TTL","chatterbox",1,4,2);
    if(!a||!b)return 6;
    obs_source_add_audio_capture_callback(a,capture,nullptr);
    auto* props=obs_source_properties(b);
    auto* ttl=obs_properties_get(props,"retention_ttl");auto* mode=obs_properties_get(props,"retention_mode");
    bool picker=ttl&&mode&&obs_property_visible(ttl)&&obs_property_list_item_count(mode)==3;
    auto* cfg=obs_source_get_settings(b);obs_data_set_string(cfg,"backend","edge");
    obs_property_modified(obs_properties_get(props,"backend"),cfg);
    bool cloud=!obs_property_visible(ttl)&&!obs_property_visible(mode);
    obs_data_set_string(cfg,"backend","chatterbox");obs_data_release(cfg);
    obs_properties_destroy(props);
    bool lazy=wait_model("chatterbox",2,false);
    bool loaded=lazy&&speech(a,1)&&wait_model("chatterbox",2,true);
    printf("SOUND_TTS_RETENTION_LOADED=%d\n",loaded);fflush(stdout);
    // Give the external VRAM sampler a stable observation window.
    std::this_thread::sleep_for(1000ms);
    config(a,"edge",0);
    bool shared=loaded&&wait_model("chatterbox",1,true);
    config(b,"edge",1,4);
    bool retained=shared&&wait_model("chatterbox",0,true,2500);
    std::this_thread::sleep_for(1s);
    retained=retained&&read_model("chatterbox").loaded;
    bool expired=retained&&wait_model("chatterbox",0,false,8000);
    printf("SOUND_TTS_RETENTION_EXPIRED=%d\n",expired);fflush(stdout);
    std::this_thread::sleep_for(1s);
    config(a,"chatterbox",2);
    bool reload=expired&&wait_model("chatterbox",1,false)&&speech(a,2);
    config(a,"edge",2);
    bool warm=reload&&wait_model("chatterbox",0,true);
    std::this_thread::sleep_for(1s);
    warm=warm&&read_model("chatterbox").loaded;
    config(a,"chatterbox",0);
    bool acquired=wait_model("chatterbox",1,true);
    config(a,"edge",0);
    bool immediate=acquired&&wait_model("chatterbox",0,false,4000);
    printf("SOUND_TTS_RETENTION_OBS picker=%d cloud=%d lazy=%d loaded=%d shared=%d ttl=%d expired=%d reload=%d warm=%d immediate=%d\n",
      picker,cloud,lazy,loaded,shared,retained,expired,reload,warm,immediate);
    obs_source_remove_audio_capture_callback(a,capture,nullptr);
    obs_set_output_source(1,nullptr);obs_set_output_source(2,nullptr);
    obs_source_release(a);obs_source_release(b);obs_shutdown();
    bool ok=picker&&cloud&&lazy&&loaded&&shared&&retained&&expired&&reload&&warm&&immediate;
    printf("SOUND_TTS_RETENTION_OBS_PASS=%d\n",ok);return ok?0:1;
}
