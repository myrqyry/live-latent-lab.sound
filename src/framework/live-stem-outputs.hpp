#pragma once
// OBS source bindings and output dispatch live on control/worker threads.
// No OBS API calls or shared-pointer operations in the capture callback.
#include <obs-module.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stts::stem_outputs {
inline constexpr char source_id[]="stem_live_output";
inline constexpr char filter_id[]="stem_live_buffered_filter";
inline constexpr char mel_filter_id[]="stem_live_melband_filter";
inline bool allowed_parent(obs_source_t* source){
    const auto* id=source?obs_source_get_unversioned_id(source):nullptr;
    return id&&std::strcmp(id,source_id)!=0&&
        obs_source_get_type(source)==OBS_SOURCE_TYPE_INPUT&&
        (obs_source_get_output_flags(source)&OBS_SOURCE_AUDIO);
}
struct Binding {
    Binding(obs_source_t* s,std::string key,int index)
        :weak(obs_source_get_weak_source(s)),uuid(std::move(key)),stem(index){}
    ~Binding(){if(weak)obs_weak_source_release(weak);}
    obs_weak_source_t* weak;
    const std::string uuid;
    const int stem;
    std::atomic<bool> live{true};
};
inline std::mutex registry_mutex;
inline std::atomic<uint64_t> revision{1};
inline std::vector<std::weak_ptr<Binding>> registry;
inline void add(const std::shared_ptr<Binding>& binding){
    std::lock_guard<std::mutex> lock(registry_mutex);
    registry.erase(std::remove_if(registry.begin(),registry.end(),[](const auto& p){return p.expired();}),registry.end());
    registry.emplace_back(binding);
    revision.fetch_add(1,std::memory_order_release);
}
inline std::vector<std::shared_ptr<Binding>> find(const std::string& uuid){
    std::vector<std::shared_ptr<Binding>> result;
    if(uuid.empty())return result;
    std::lock_guard<std::mutex> lock(registry_mutex);
    for(auto& p:registry)if(auto b=p.lock();b&&b->uuid==uuid&&b->live.load(std::memory_order_acquire))
        result.push_back(std::move(b));
    return result;
}
struct Output {
    obs_source_t* source;
    std::mutex control;
    std::shared_ptr<Binding> binding;
};
inline void update(void* ptr,obs_data_t* settings){
    auto* d=static_cast<Output*>(ptr);
    if(!d)return;
    auto* id=obs_data_get_string(settings,"filter_uuid");
    std::string key=id?id:"";
    int stem=int(obs_data_get_int(settings,"stem"));
    if(stem<0||stem>4)stem=3;
    std::lock_guard<std::mutex> lock(d->control);
    if(d->binding&&d->binding->uuid==key&&d->binding->stem==stem)return;
    if(d->binding){d->binding->live.store(false,std::memory_order_release);revision.fetch_add(1,std::memory_order_release);}
    d->binding.reset();
    if(!key.empty()&&key.size()<=80){
        d->binding=std::make_shared<Binding>(d->source,std::move(key),stem);
        add(d->binding);
    }
}
inline const char* name(void*){return "LLL.sound Stem Output";}
inline void defaults(obs_data_t* settings){obs_data_set_default_int(settings,"stem",3);}
inline void* create(obs_data_t* settings,obs_source_t* source){
    try{
        auto d=std::make_unique<Output>();d->source=source;
        update(d.get(),settings);
        return d.release();
    }catch(...){return nullptr;}
}
inline void destroy(void* ptr){
    auto* d=static_cast<Output*>(ptr);if(!d)return;
    if(d->binding){d->binding->live.store(false,std::memory_order_release);revision.fetch_add(1,std::memory_order_release);}
    delete d;
}
inline void add_filter(obs_source_t* parent,obs_source_t* filter,void* ptr){
    const auto* id=obs_source_get_unversioned_id(filter);
    const auto* uuid=obs_source_get_uuid(filter);
    if(!id||(std::strcmp(id,filter_id)&&std::strcmp(id,mel_filter_id))||!uuid||!*uuid||obs_source_removed(filter))return;
    auto label=std::string(obs_source_get_name(parent))+" / "+obs_source_get_name(filter);
    obs_property_list_add_string(static_cast<obs_property_t*>(ptr),label.c_str(),uuid);
}
inline bool add_parent(void* ptr,obs_source_t* parent){
    if(allowed_parent(parent)&&!obs_source_removed(parent))obs_source_enum_filters(parent,add_filter,ptr);
    return true;
}
struct ParentProfile{std::string key;int profile=-1;};
inline void profile_filter(obs_source_t*,obs_source_t* filter,void* ptr){
    auto* p=static_cast<ParentProfile*>(ptr);
    auto* uuid=obs_source_get_uuid(filter);auto* id=obs_source_get_unversioned_id(filter);
    if(!uuid||!id||p->key!=uuid)return;
    if(!std::strcmp(id,mel_filter_id))p->profile=1;
    else if(!std::strcmp(id,filter_id))p->profile=0;
}
inline bool profile_parent(void* ptr,obs_source_t* parent){
    if(allowed_parent(parent)&&!obs_source_removed(parent))obs_source_enum_filters(parent,profile_filter,ptr);
    return static_cast<ParentProfile*>(ptr)->profile<0;
}
inline bool parent_changed(obs_properties_t* props,obs_property_t*,obs_data_t* settings){
    ParentProfile profile{obs_data_get_string(settings,"filter_uuid")};
    obs_enum_sources(profile_parent,&profile);
    auto* stem=obs_properties_get(props,"stem");
    if(stem)for(size_t i=0;i<obs_property_list_item_count(stem);++i){
        auto id=obs_property_list_item_int(stem,i);
        obs_property_list_item_disable(stem,i,profile.profile==1?id<3:profile.profile==0?id==4:false);
    }
    return true;
}
inline obs_properties_t* properties(void*){
    auto* p=obs_properties_create();
    auto* parents=obs_properties_add_list(p,"filter_uuid","Shared stem filter",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(parents,"(Select a buffered stem filter)","");
    obs_enum_sources(add_parent,parents);
    obs_property_set_modified_callback(parents,parent_changed);
    auto* stem=obs_properties_add_list(p,"stem","Stereo stem",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(stem,"Drums",0);obs_property_list_add_int(stem,"Bass",1);
    obs_property_list_add_int(stem,"Other",2);obs_property_list_add_int(stem,"Vocals",3);
    obs_property_list_add_int(stem,"Instrumental (RoFormer)",4);
    obs_properties_add_text(p,"note","Uses an existing LLL.sound Buffered stem filter. Add as a normal scene source; select Separate outputs on the parent to avoid duplicating its mix. Use OBS mixer for volume, monitoring and tracks. Keep the parent input active. Unbound or unavailable outputs are silent.",OBS_TEXT_INFO);
    return p;
}
}
