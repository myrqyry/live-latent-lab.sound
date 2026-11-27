// Linux test-only probe for OBS source-capture callbacks registered by Sound.
// A registration owns its own statistics. Fixture callbacks are untouched.
#pragma once
#include "rt-callback-probe.hpp"
#include <mutex>
#include <cassert>
namespace rt_capture_probe {
struct Slot {
    obs_source_t* source=nullptr;
    obs_source_audio_capture_t fn=nullptr;
    void* param=nullptr;
    bool used=false,attached=false;
    rt_probe::Stats stats{nullptr,"sound_capture"};
};
inline std::array<Slot,8> slots{};
inline std::mutex registrations;
inline bool from_sound(obs_source_audio_capture_t fn){
    Dl_info info{};
    return dladdr(reinterpret_cast<void*>(fn),&info)&&info.dli_fname&&
        std::strcmp(std::strrchr(info.dli_fname,'/')?std::strrchr(info.dli_fname,'/')+1:info.dli_fname,
                    "live-latent-lab-sound.so")==0;
}
inline void capture(void* p,obs_source_t* src,const audio_data* a,bool muted){
    auto& slot=*static_cast<Slot*>(p);
    rt_probe::measure(slot.stats,[&]{slot.fn(slot.param,src,a,muted);});
}
inline bool report(size_t minimum){
    // Report only after all source-capture sessions have detached.
    std::lock_guard guard(registrations);
    size_t count=0;bool ok=true;
    for(auto& slot:slots)if(slot.used){
        ++count;ok=!slot.attached&&rt_probe::report_one(slot.stats,minimum)&&ok;
    }
    return count>0&&ok;
}
}
extern "C" {
void obs_source_add_audio_capture_callback(obs_source_t* source,obs_source_audio_capture_t fn,void* param){
    using Fn=void(*)(obs_source_t*,obs_source_audio_capture_t,void*);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"obs_source_add_audio_capture_callback"));
    if(!rt_capture_probe::from_sound(fn)){real(source,fn,param);return;}
    std::lock_guard guard(rt_capture_probe::registrations);
    for(auto& slot:rt_capture_probe::slots)if(!slot.used){
        slot.source=source;slot.fn=fn;slot.param=param;slot.used=slot.attached=true;
        real(source,rt_capture_probe::capture,&slot);return;
    }
    std::abort(); // Never silently omit a callback when a test exceeds capacity.
}
void obs_source_remove_audio_capture_callback(obs_source_t* source,obs_source_audio_capture_t fn,void* param){
    using Fn=void(*)(obs_source_t*,obs_source_audio_capture_t,void*);
    static const auto real=reinterpret_cast<Fn>(dlsym(RTLD_NEXT,"obs_source_remove_audio_capture_callback"));
    if(!rt_capture_probe::from_sound(fn)){real(source,fn,param);return;}
    std::lock_guard guard(rt_capture_probe::registrations);
    for(auto& slot:rt_capture_probe::slots)if(slot.attached&&slot.source==source&&slot.fn==fn&&slot.param==param){
        // libobs removes under the same mutex used to invoke callbacks, so
        // the callback has finished when this call returns.
        real(source,rt_capture_probe::capture,&slot);slot.attached=false;return;
    }
    std::abort();
}
}
