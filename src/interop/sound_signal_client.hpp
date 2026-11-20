#pragma once
// Versioned Sound transport for in-process OBS plugins. Only libobs is
// needed. Metadata discovery belongs on the control thread; read on a
// control/worker thread, never the audio callback. No model ownership or
// C++ objects cross module boundaries. All channels are stable UUIDs.
#include <obs.h>
#include <callback/proc.h>
#include <cstring>
#include <string>
#include <vector>
namespace sound_signals {
enum class Kind {Unknown,Transcript,Classification,Speech};
inline const char* kind_name(Kind kind){
    switch(kind){case Kind::Transcript:return "transcript";case Kind::Classification:return "classification";case Kind::Speech:return "speech";default:return "unknown";}
}
inline Kind kind(obs_source_t* s){
    const auto* id=s?obs_source_get_unversioned_id(s):nullptr;
    if(!id)return Kind::Unknown;
    if(!strcmp(id,"sound_stt_filter"))return Kind::Transcript;
    if(!strcmp(id,"yamnet_classifier_source"))return Kind::Classification;
    for(auto name:{"sound_tts_source","sound_kokoro_tts_source","sound_piper_tts_source","sound_chatterbox_tts_source","sound_tiktok_tts_source"})
        if(!strcmp(id,name))return Kind::Speech;
    return Kind::Unknown;
}
inline bool relay_parent(obs_source_t* source){
    if(!source||obs_source_get_type(source)!=OBS_SOURCE_TYPE_INPUT||
       !(obs_source_get_output_flags(source)&OBS_SOURCE_AUDIO)||kind(source)==Kind::Speech)return false;
    auto* id=obs_source_get_unversioned_id(source);
    return id&&strcmp(id,"kokoro_tts_source")&&strcmp(id,"chatterbox_source");
}
inline std::string channel(obs_source_t* parent,obs_source_t* filter=nullptr){
    if(!parent||!obs_source_get_uuid(parent))return {};
    std::string key=obs_source_get_uuid(parent);
    if(filter){auto* id=obs_source_get_uuid(filter);if(!id)return {};key+="/";key+=id;}
    return key;
}
struct Endpoint {
    std::string channel,label;
    Kind kind=Kind::Unknown;
    bool enabled=false,relay_safe=false;
};
inline void discover_filter(obs_source_t* parent,obs_source_t* filter,void* opaque){
    auto k=kind(filter);
    if(k!=Kind::Transcript&&k!=Kind::Classification)return;
    auto id=channel(parent,filter);if(id.empty())return;
    auto& out=*static_cast<std::vector<Endpoint>*>(opaque);
    out.push_back({id,std::string(obs_source_get_name(parent))+" / "+obs_source_get_name(filter),k,
        obs_source_enabled(parent)&&obs_source_enabled(filter)&&!obs_source_removed(parent)&&!obs_source_removed(filter),relay_parent(parent)});
}
inline bool discover_source(void* opaque,obs_source_t* source){
    if(obs_source_removed(source))return true;
    auto& out=*static_cast<std::vector<Endpoint>*>(opaque);
    if(kind(source)==Kind::Speech)
        out.push_back({channel(source),obs_source_get_name(source),Kind::Speech,obs_source_enabled(source),false});
    obs_source_enum_filters(source,discover_filter,opaque);
    return true;
}
inline std::vector<Endpoint> discover(){
    std::vector<Endpoint> out;
    obs_enum_sources(discover_source,&out);
    return out;
}
struct Search {
    std::string uuid;
    Kind expected;
    obs_source_t* ref=nullptr;
};
inline void find_filter(obs_source_t*,obs_source_t* filter,void* data){
    auto& search=*static_cast<Search*>(data);
    auto* id=obs_source_get_uuid(filter);
    const auto k=kind(filter);
    if(!search.ref&&id&&search.uuid==id&&(k==Kind::Transcript||k==Kind::Classification)&&
       (search.expected==Kind::Unknown||search.expected==k)&&
       !obs_source_removed(filter)&&obs_source_enabled(filter))
        search.ref=obs_source_get_ref(filter);
}
// Caller releases the strong reference. Never use a borrowed filter parent.
inline obs_source_t* resolve(const std::string& key,Kind expected=Kind::Unknown,bool relay_only=false){
    if(key.empty()||key.size()>160)return nullptr;
    auto slash=key.find('/');
    auto* parent=obs_get_source_by_uuid(key.substr(0,slash).c_str());
    if(!parent)return nullptr;
    if(obs_source_removed(parent)||!obs_source_enabled(parent)){
        obs_source_release(parent);return nullptr;
    }
    if(slash==std::string::npos){
        if(!relay_only&&kind(parent)==Kind::Speech&&(expected==Kind::Unknown||expected==Kind::Speech))return parent;
        obs_source_release(parent);return nullptr;
    }
    Search search{key.substr(slash+1),expected};
    if(!search.uuid.empty()&&(!relay_only||relay_parent(parent)))
        obs_source_enum_filters(parent,find_filter,&search);
    obs_source_release(parent);return search.ref;
}
struct Transcript {
    bool connected=false,available=false,final=false;
    uint64_t sequence=0,revision=0;
    std::string text,status;
};
inline Transcript read_transcript(const std::string& key,bool final_only=true,bool relay_only=false){
    Transcript out;
    auto* source=resolve(key,Kind::Transcript,relay_only);
    if(!source)return out;
    calldata_t call{};
    if(proc_handler_call(obs_source_get_proc_handler(source),final_only?"sound_stt_read_v1":"sound_stt_read_v2",&call)){
        if(auto* status=calldata_string(&call,"status"))out.status=status;
        out.connected=!out.status.empty()&&out.status!="off";
        auto seq=calldata_int(&call,"sequence");
        auto rev=calldata_int(&call,"revision");
        out.sequence=seq>0?uint64_t(seq):0;out.revision=final_only?out.sequence:rev>0?uint64_t(rev):0;
        out.available=calldata_bool(&call,"available");
        out.final=out.available&&(final_only||calldata_bool(&call,"final"));
        if(out.available){if(auto* text=calldata_string(&call,"text"))out.text=text;}
        if(out.text.empty()){out.available=false;out.final=false;}
    }
    calldata_free(&call);obs_source_release(source);return out;
}
// Independently compiled clients can read all current snapshot types
// through this same helper, or use the broker procedure on any endpoint.
inline void read(const std::string& key,const std::string& stream,const std::string& category,calldata_t* out){
    calldata_set_bool(out,"available",false);calldata_set_bool(out,"connected",false);calldata_set_bool(out,"final",false);
    calldata_set_string(out,"kind","unknown");calldata_set_string(out,"text","");calldata_set_string(out,"status","unavailable");
    calldata_set_string(out,"label","");calldata_set_float(out,"score",0);
    for(auto field:{"sequence","revision","completed","failed"})calldata_set_int(out,field,0);
    auto* source=resolve(key);if(!source)return;
    auto k=kind(source);calldata_set_string(out,"kind",kind_name(k));
    if(k==Kind::Transcript&&(stream=="final"||stream=="latest")){
        obs_source_release(source);
        auto t=read_transcript(key,stream=="final");
        calldata_set_bool(out,"connected",t.connected);calldata_set_bool(out,"available",t.available);
        calldata_set_bool(out,"final",t.final);calldata_set_int(out,"sequence",t.sequence);calldata_set_int(out,"revision",t.revision);
        calldata_set_string(out,"text",t.text.c_str());calldata_set_string(out,"status",t.status.c_str());return;
    }
    calldata_t result{};
    auto* handler=obs_source_get_proc_handler(source);
    if(k==Kind::Classification&&stream=="latest"&&proc_handler_call(handler,"sound_yamnet_read_v1",&result)){
        auto available=calldata_bool(&result,"available");
        calldata_set_bool(out,"connected",true);calldata_set_bool(out,"available",available);
        if(auto* value=calldata_string(&result,"status"))calldata_set_string(out,"status",value);
        if(available){
            if(auto* value=calldata_string(&result,"label"))calldata_set_string(out,"label",value);
            calldata_set_float(out,"score",calldata_float(&result,"score"));
        }
        calldata_set_int(out,"sequence",calldata_int(&result,"windows"));
    }else if(k==Kind::Classification&&stream=="category"&&!category.empty()&&category.size()<=256){
        calldata_set_string(&result,"category",category.c_str());
        if(proc_handler_call(handler,"sound_yamnet_score_v1",&result)){
            auto available=calldata_bool(&result,"available");
            calldata_set_bool(out,"connected",true);calldata_set_bool(out,"available",available);
            calldata_set_string(out,"status",available?"ready":"unavailable");
            if(available){calldata_set_string(out,"label",category.c_str());calldata_set_float(out,"score",calldata_float(&result,"score"));}
        }
    }else if(k==Kind::Speech&&stream=="latest"&&proc_handler_call(handler,"sound_tts_status_v1",&result)){
        calldata_set_bool(out,"connected",true);calldata_set_bool(out,"available",true);calldata_set_string(out,"status","ready");
        calldata_set_int(out,"completed",calldata_int(&result,"completed"));calldata_set_int(out,"failed",calldata_int(&result,"failed"));
    }
    calldata_free(&result);obs_source_release(source);
}
inline void export_inventory(obs_data_t* out){
    auto* rows=obs_data_array_create();
    for(const auto& e:discover()){
        if(e.channel.empty())continue;
        auto* row=obs_data_create();
        obs_data_set_string(row,"channel",e.channel.c_str());obs_data_set_string(row,"label",e.label.c_str());
        obs_data_set_string(row,"kind",kind_name(e.kind));obs_data_set_bool(row,"enabled",e.enabled);
        obs_data_set_bool(row,"relay_safe",e.relay_safe);
        obs_data_set_string(row,"read_proc",e.kind==Kind::Transcript?"sound_stt_read_v2":e.kind==Kind::Classification?"sound_yamnet_read_v1":"sound_tts_status_v1");
        obs_data_set_string(row,"write_proc",e.kind==Kind::Speech?"sound_tts_enqueue_v1":"");
        obs_data_array_push_back(rows,row);obs_data_release(row);
    }
    obs_data_set_int(out,"version",1);obs_data_set_array(out,"channels",rows);obs_data_array_release(rows);
}
inline void list_proc(void*,calldata_t* args){
    void* out=nullptr;
    if(calldata_get_ptr(args,"inventory",&out)&&out)export_inventory(static_cast<obs_data_t*>(out));
}
inline void read_proc(void*,calldata_t* args){
    const auto get=[&](const char* field){auto* v=calldata_string(args,field);return std::string(v?v:"");};
    auto key=get("channel"),stream=get("stream"),category=get("category");
    read(key,stream,category,args);
}
inline void register_broker(obs_source_t* source){
    auto* h=obs_source_get_proc_handler(source);
    proc_handler_add(h,"void sound_signal_list_v1(in ptr inventory)",list_proc,nullptr);
    proc_handler_add(h,"void sound_signal_read_v1(in string channel, in string stream, in string category, out string kind, out bool connected, out bool available, out string status, out string text, out bool final, out int sequence, out int revision, out string label, out float score, out int completed, out int failed)",read_proc,nullptr);
}
inline bool enqueue(const std::string& channel,const std::string& text){
    auto* source=resolve(channel,Kind::Speech);if(!source)return false;
    calldata_t call{};calldata_set_string(&call,"text",text.c_str());
    bool accepted=proc_handler_call(obs_source_get_proc_handler(source),"sound_tts_enqueue_v1",&call)&&calldata_bool(&call,"accepted");
    calldata_free(&call);obs_source_release(source);return accepted;
}
} // namespace sound_signals
