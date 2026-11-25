#include <obs.h>
#include <obs-module.h>
#include <cstdio>
#include <string>

int main(int argc,char** argv){
    if(argc!=3||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_module_t* m=nullptr;
    if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 4;
    auto* s=obs_source_create("sound_audio_diffusion_source","Refresh fixture",nullptr,nullptr);
    if(!s)return 5;
    // Keep the existing property object: OBS may rebuild its widgets from
    // this object instead of requesting a new source property list.
    auto* p=obs_source_properties(s);
    auto* note=obs_properties_get(p,"note");
    auto* refresh=obs_properties_get(p,"refresh_status");
    auto* stop=obs_properties_get(p,"stop");
    std::string before=note?obs_property_description(note):"";
    bool clicked=stop&&refresh;
    if(clicked){
        obs_property_button_clicked(stop,s);
        obs_property_button_clicked(refresh,s);
    }
    const std::string after=note?obs_property_description(note):"";
    bool ok=clicked&&before.find("Status: Ready")!=std::string::npos&&
        after.find("Status: Stopped")!=std::string::npos&&after.find("Current buffer: 0 ms")!=std::string::npos;
    printf("SA3_STATUS_REFRESH clicked=%d changed=%d pass=%d\n",clicked,before!=after,ok);
    obs_properties_destroy(p);obs_source_release(s);obs_wait_for_destroy_queue();obs_shutdown();
    return ok?0:1;
}
