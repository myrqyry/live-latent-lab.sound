#pragma once
#include <obs.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <thread>

// Optional real overlapping audio generation and recognition in the same
// libobs process. The STT harness still asserts the recognized words,
// caption routing and dry-audio equality independently.
struct Sa3SttCoexist {
    obs_source_t* source=nullptr;
    const char* models=std::getenv("SOUND_TEST_SA3_MODELS");
    std::atomic<uint64_t> nonzero{0};
    static void capture(void* p,obs_source_t*,const audio_data* a,bool){
        auto& s=*static_cast<Sa3SttCoexist*>(p);
        if(!a->data[0])return;
        auto* f=reinterpret_cast<const float*>(a->data[0]);
        uint64_t nz=0;for(unsigned i=0;i<a->frames;i++)if(std::isfinite(f[i])&&std::abs(f[i])>.001f)++nz;
        s.nonzero+=nz;
    }
    int64_t status(const char* key){
        if(!source)return -1;
        calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(source),"sound_diffusion_status_v1",&d);
        auto n=ok?calldata_int(&d,key):-1;calldata_free(&d);return n;
    }
    bool start(){
        if(!models)return true;
        auto* d=obs_data_create();obs_data_set_string(d,"models_dir",models);
        obs_data_set_string(d,"prompt","soft instrumental toy keyboard, bass and drums");
        obs_data_set_double(d,"duration",6);obs_data_set_int(d,"steps",32);obs_data_set_int(d,"seed",42);
        source=obs_source_create("sound_audio_diffusion_source","Coexist music",d,nullptr);obs_data_release(d);
        if(!source)return false;
        obs_source_add_audio_capture_callback(source,capture,this);
        obs_set_output_source(2,source);obs_source_set_audio_mixers(source,1);obs_source_set_audio_active(source,true);
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        auto* p=obs_source_properties(source);auto* b=obs_properties_get(p,"generate");
        if(b)obs_property_button_clicked(b,source);
        obs_properties_destroy(p);
        if(!b)return false;
        for(int i=0;i<100&&status("started")<1;i++)std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return status("started")==1;
    }
    bool finish(){
        if(!models)return true;
        if(!source)return false;
        for(int i=0;i<750&&status("played")<1&&status("failed")==0;i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        bool ok=status("completed")==1&&status("played")==1&&status("failed")==0&&nonzero>1000;
        printf("SOUND_DIFFUSION_STT_COEXIST played=%lld samples=%llu pass=%d\n",
               (long long)status("played"),(unsigned long long)nonzero.load(),ok);
        obs_source_remove_audio_capture_callback(source,capture,this);
        obs_set_output_source(2,nullptr);obs_source_release(source);obs_wait_for_destroy_queue();source=nullptr;
        return ok;
    }
};
