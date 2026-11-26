#include "framework/preferences.hpp"
// Optional monophonic neural timbre transfer. All model loading and synthesis
// occur on the worker. OBS receives a fixed-delay dry/wet stream.
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <openssl/evp.h>
#include "framework/ddsp_pipeline.hpp"
#include "engines/ddsp_litert.hpp"
#include "engines/ddsp_manifest.hpp"

namespace {
enum Status{Loading,Ready,Error};
struct Filter{
    explicit Filter(size_t rate):pipeline(rate){}
    ddsp::Pipeline pipeline;
    std::atomic<bool> stop{false};
    std::atomic<int> status{Loading};
    std::atomic<float> mix{1},morph{0},pitch{0},harmonics{1},noise{1},expression{1},gate{-60},gain{1};
    std::atomic<float> hz{0},db{-80},inference_ms{0};
    std::atomic<uint64_t> hops{0};
    bool started=false; // audio thread only
    std::mutex config_mutex; // settings and worker only
    std::string lib,models;
    uint64_t config_revision=0;
    std::atomic<int> a{9},b{2};
    std::thread worker;
};
std::string verified(const std::filesystem::path& path,const ddsp::Asset& asset){
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)!=asset.bytes)
        throw std::runtime_error("Missing or wrong size DDSP asset");
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("DDSP asset open");
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
    if(!ctx||EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1)throw std::runtime_error("DDSP checksum init");
    std::array<char,65536> buf{};size_t bytes=0;
    while(f){f.read(buf.data(),buf.size());auto n=f.gcount();if(n>0){bytes+=size_t(n);if(bytes>asset.bytes||EVP_DigestUpdate(ctx.get(),buf.data(),size_t(n))!=1)throw std::runtime_error("DDSP checksum read");}}
    if(f.bad()||bytes!=asset.bytes)throw std::runtime_error("DDSP asset changed");
    unsigned char hash[32];unsigned len=0;
    if(EVP_DigestFinal_ex(ctx.get(),hash,&len)!=1||len!=32)throw std::runtime_error("DDSP checksum final");
    char hex[65]={};for(size_t i=0;i<32;++i)std::snprintf(hex+2*i,3,"%02x",hash[i]);
    if(std::string(hex)!=asset.sha)throw std::runtime_error("Unqualified DDSP asset checksum");
    return path.string();
}
int instrument(const std::string& name){
    for(size_t i=0;i<std::size(ddsp::instruments);++i)if(name==ddsp::instruments[i].name)return int(i);
    return -1;
}
void run(Filter* f){
    // Failed initial asset selection can be corrected in the property dialog.
    // Once ready, the asset root is fixed for this filter lifetime. The 11
    // preloaded models are shared across the two lanes; each live lane has
    // its own recurrent state, except identical A/B uses one evaluation.
    while(!f->stop.load(std::memory_order_acquire)){
        std::string lib,models;uint64_t revision;
        {
            std::lock_guard<std::mutex> lock(f->config_mutex);
            lib=f->lib;models=f->models;revision=f->config_revision;
        }
        bool initialized=false;
        try{
            f->status.store(Loading,std::memory_order_release);
            auto path=verified(lib,ddsp::runtime_asset);
            auto root=std::filesystem::path(models);
            auto pf=verified(root/"extract_features_micro.tflite",ddsp::feature_asset);
            ddsp::Runtime runtime(path);
            ddsp::Features features(runtime,pf);
            std::array<std::unique_ptr<ddsp::Decoder>,std::size(ddsp::instruments)> decoders;
            std::array<float,1024> zero{};auto first=features.run(zero);
            for(size_t i=0;i<decoders.size();++i){
                if(f->stop.load())return;
                auto& asset=ddsp::instruments[i];
                auto p=verified(root/(std::string(asset.name)+".tflite"),asset);
                decoders[i]=std::make_unique<ddsp::Decoder>(runtime,p);
                decoders[i]->run(first);decoders[i]->reset();
            }
            ddsp::Synth synth;ddsp::Analysis analysis(f->pipeline.hop()*50);ddsp::Upsample up;
            std::vector<float> l(f->pipeline.hop()),r(l.size()),out(l.size());
            std::array<float,320> rendered{};
            {
                std::lock_guard<std::mutex> lock(f->config_mutex);
                if(revision!=f->config_revision)continue;
                initialized=true;
                f->status.store(Ready,std::memory_order_release);
            }
            int old_a=-1,old_b=-1;
            while(!f->stop.load(std::memory_order_acquire)&&!f->pipeline.failed()){
                if(!f->pipeline.read(l.data(),r.data())){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
                auto start=std::chrono::steady_clock::now();
                auto feature=features.run(analysis.push(l.data(),r.data()));
                f->hz.store(feature.db>f->gate.load()?feature.hz:0);
                f->db.store(feature.db);
                feature.hz=std::clamp(feature.hz*std::pow(2.f,f->pitch.load()/12.f),20.f,4000.f);
                feature.scaled=std::clamp(float(69.+12.*std::log2(feature.hz/440.))/127.f,0.f,1.f);
                feature.power=std::clamp(.65f+(feature.power-.65f)*f->expression.load(),0.f,1.f);
                int a=f->a.load(),b=f->b.load();
                if(a!=old_a&&a!=old_b)decoders[a]->reset();
                if(b!=a&&b!=old_a&&b!=old_b)decoders[b]->reset();
                auto ca=decoders[a]->run(feature);
                auto c=a==b?ca:ddsp::blend(ca,decoders[b]->run(feature),f->morph.load());
                old_a=a;old_b=b;
                if(feature.db<f->gate.load())c={};
                synth.render(c,feature.hz,f->harmonics.load(),f->noise.load(),rendered);
                up.run(rendered,out.data(),out.size());float gain=f->gain.load();
                for(auto& v:out)v=std::clamp(v*gain,-.95f,.95f);
                f->inference_ms.store(float(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()));
                if(!f->pipeline.write(out.data()))break;
                ++f->hops;
            }
            if(f->pipeline.failed())f->status.store(Error);
            return;
        }catch(const std::exception& e){
            blog(LOG_WARNING,"[Sound DDSP] %s",e.what());f->status.store(Error);
            if(initialized){f->pipeline.fail();return;}
        }catch(...){
            f->status.store(Error);if(initialized){f->pipeline.fail();return;}
        }
        // Wait for an explicit settings change, not an endless load loop.
        while(!f->stop.load()){
            {std::lock_guard<std::mutex> lock(f->config_mutex);if(revision!=f->config_revision)break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
const char* name(void*){return "LLL.sound Neural Timbre (DDSP)";}
float number(obs_data_t* d,const char* k,float fallback,float low,float high){
    double v=obs_data_get_double(d,k);return std::isfinite(v)?std::clamp(float(v),low,high):fallback;
}
void defaults(obs_data_t* d){
    obs_data_set_default_string(d,"instrument_a","Violin");obs_data_set_default_string(d,"instrument_b","Flute");
    obs_data_set_default_double(d,"mix",1);obs_data_set_default_double(d,"morph",0);obs_data_set_default_double(d,"pitch",0);
    obs_data_set_default_double(d,"harmonics",1);obs_data_set_default_double(d,"noise",1);obs_data_set_default_double(d,"expression",1);
    obs_data_set_default_double(d,"gate",-60);obs_data_set_default_double(d,"gain",1);
    if(const char* home=std::getenv("HOME")){
        const std::string root=std::string(home)+"/.local/share/live-latent-lab-sound/ddsp";
        obs_data_set_default_string(d,"runtime",(root+"/libLiteRt.so").c_str());
        obs_data_set_default_string(d,"models",(root+"/models").c_str());
    }
    if(const char* path=std::getenv("SOUND_DDSP_LIBRARY"))obs_data_set_default_string(d,"runtime",path);
    if(const char* path=std::getenv("SOUND_DDSP_MODELS"))obs_data_set_default_string(d,"models",path);
}
void update(void* data,obs_data_t* d){
    auto* f=static_cast<Filter*>(data);if(!f)return;
    f->mix=number(d,"mix",1,0,1);f->morph=number(d,"morph",0,0,1);f->pitch=number(d,"pitch",0,-24,24);
    f->harmonics=number(d,"harmonics",1,0,2);f->noise=number(d,"noise",1,0,2);f->expression=number(d,"expression",1,0,1);
    f->gate=number(d,"gate",-60,-80,-20);f->gain=number(d,"gain",1,0,3);
    int a=instrument(obs_data_get_string(d,"instrument_a")),b=instrument(obs_data_get_string(d,"instrument_b"));
    if(a>=0)f->a.store(a);if(b>=0)f->b.store(b);
    std::lock_guard<std::mutex> lock(f->config_mutex);
    std::string lib=obs_data_get_string(d,"runtime"),models=obs_data_get_string(d,"models");
    if(f->status.load()!=Ready&&(lib!=f->lib||models!=f->models)){
        f->lib=std::move(lib);f->models=std::move(models);++f->config_revision;
    }
}
obs_properties_t* properties(void* data){
    auto* p=obs_properties_create();
    obs_properties_add_path(p,"runtime","LiteRT CPU library",OBS_PATH_FILE,"Shared library (*.so)",sound_preferences::directory(true).c_str());
    obs_properties_add_path(p,"models","DDSP model directory",OBS_PATH_DIRECTORY,nullptr,sound_preferences::directory(false).c_str());
    for(auto key:{"instrument_a","instrument_b"}){
        auto* list=obs_properties_add_list(p,key,std::strcmp(key,"instrument_a")==0?"Timbre A":"Timbre B",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
        for(auto& a:ddsp::instruments)obs_property_list_add_string(list,a.name,a.name);
    }
    obs_properties_add_text(p,"note","Monophonic input; 200 ms fixed delay after loading. Timbres switch live. Runtime and model folder can be corrected before a successful load; changing them after load requires reopening the scene. A missed deadline falls back to aligned delayed dry until recreated.",OBS_TEXT_INFO);
    if(auto* f=static_cast<Filter*>(data)){
        auto status=f->status.load();
        obs_properties_add_text(p,"status",f->pipeline.failed()?"Processing stopped; delayed dry":status==Ready?"Ready: CPU / 11 timbres":status==Loading?"Loading models; dry audio":"Model error: correct paths; dry audio",OBS_TEXT_INFO);
    }
    obs_properties_add_float_slider(p,"morph","Timbre morph (A → B)",0,1,.01);
    obs_properties_add_float_slider(p,"pitch","Pitch (semitones)",-24,24,1);
    obs_properties_add_float_slider(p,"harmonics","Harmonic gain",0,2,.01);
    obs_properties_add_float_slider(p,"noise","Noise texture",0,2,.01);
    obs_properties_add_float_slider(p,"expression","Expression tracking",0,1,.01);
    obs_properties_add_float_slider(p,"gate","Silence gate (dB)",-80,-20,1);
    obs_properties_add_float_slider(p,"gain","Wet output gain",0,3,.01);
    obs_properties_add_float_slider(p,"mix","Wet / dry",0,1,.01);
    return p;
}
void snapshot(void* data,calldata_t* out){
    auto* f=static_cast<Filter*>(data);int status=f->status.load();
    if(f->pipeline.failed())status=Error;
    calldata_set_string(out,"status",status==Ready?"ready":status==Loading?"loading":"error");
    calldata_set_int(out,"hops",f->hops.load());calldata_set_float(out,"pitch_hz",f->hz.load());calldata_set_float(out,"power_db",f->db.load());
    calldata_set_float(out,"inference_ms",f->inference_ms.load());calldata_set_int(out,"delay_ms",200);
}
void* create(obs_data_t* d,obs_source_t* source){
    auto* audio=obs_get_audio();if(!audio)return nullptr;
    auto rate=audio_output_get_sample_rate(audio);auto channels=audio_output_get_channels(audio);
    if((rate!=44100&&rate!=48000)||(channels!=1&&channels!=2))return nullptr;
    try{
        auto f=std::make_unique<Filter>(rate);
        update(f.get(),d);
        f->worker=std::thread(run,f.get());
        proc_handler_add(obs_source_get_proc_handler(source),"void sound_ddsp_status_v1(out string status, out int hops, out float pitch_hz, out float power_db, out float inference_ms, out int delay_ms)",snapshot,f.get());
        return f.release();
    }catch(...){return nullptr;}
}
void destroy(void* data){auto* f=static_cast<Filter*>(data);if(!f)return;f->stop=true;if(f->worker.joinable())f->worker.join();delete f;}
obs_audio_data* filter_audio(void* data,obs_audio_data* p){
    auto* f=static_cast<Filter*>(data);if(!f||!p||!p->data[0]||!p->frames)return p;
    // Stay dry before successful model initialization. Once latency begins,
    // preserve it even on worker failure. No model or filesystem work here.
    if(!f->started){if(f->status.load(std::memory_order_acquire)!=Ready)return p;f->started=true;}
    f->pipeline.process(reinterpret_cast<float*>(p->data[0]),reinterpret_cast<float*>(p->data[1]),p->frames,f->mix.load());
    return p;
}
}
void register_sound_ddsp(){
    obs_source_info info{};info.id="sound_ddsp_filter";info.type=OBS_SOURCE_TYPE_FILTER;info.output_flags=OBS_SOURCE_AUDIO;
    info.get_name=name;info.create=create;info.destroy=destroy;info.get_defaults=defaults;info.get_properties=properties;info.update=update;info.filter_audio=filter_audio;obs_register_source(&info);
}
