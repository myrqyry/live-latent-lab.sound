#include <obs.h>
#include <obs-module.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
int main(int argc,char** argv){
    if(argc!=3||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_module_t* m=nullptr;
    if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 3;
    auto* p=obs_get_source_properties("sound_stt_filter");
    if(!p)return 4;
    auto* d=obs_data_create();
    bool ok=true;
    std::vector<std::string> backends={"whisper","parakeet","nemotron"};
    auto* selector=obs_properties_get(p,"backend");
    for(size_t i=0;selector&&i<obs_property_list_item_count(selector);++i)
        if(std::string(obs_property_list_item_string(selector,i))=="zipformer")backends.emplace_back("zipformer");
    for(const auto& name:backends){
        auto* backend=name.c_str();
        obs_data_set_string(d,"backend",backend);
        obs_property_modified(obs_properties_get(p,"backend"),d);
        std::vector<std::string> names;
        for(auto* prop=obs_properties_first(p);prop;obs_property_next(&prop)){
            if(obs_property_visible(prop))names.emplace_back(obs_property_description(prop));
        }
        bool match=names.size()>=5&&names[0]=="Speech recognition engine"&&
            names[1]=="Model"&&names[2]=="Compute device"&&names[3]=="Caption text source";
        for(auto key:{"whisper_decode","zipformer_decode"}){auto* group=obs_properties_get(p,key);if(group)match=match&&(obs_property_visible(group)==(name==std::string(key).substr(0,std::string(key).find("_"))));}
        printf("STT_UNIFIED_UI backend=%s pass=%d\n",backend,match);
        ok&=match;
    }
    obs_data_release(d);obs_properties_destroy(p);obs_shutdown();
    return ok?0:1;
}
