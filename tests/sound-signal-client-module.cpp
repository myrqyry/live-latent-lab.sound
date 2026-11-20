// Independent OBS plugin used only for interop acceptance. It links libobs
// and the public client header, never the Sound binary or internal State.
#include <obs-module.h>
#include "interop/sound_signal_client.hpp"
OBS_DECLARE_MODULE()
namespace {
void submit(void*,calldata_t* call){
    auto* key=calldata_string(call,"channel");auto* text=calldata_string(call,"text");
    calldata_set_bool(call,"accepted",key&&text&&sound_signals::enqueue(key,text));
}
void* create(obs_data_t*,obs_source_t* source){
    sound_signals::register_broker(source);
    proc_handler_add(obs_source_get_proc_handler(source),
        "void probe_submit_v1(in string channel, in string text, out bool accepted)",submit,nullptr);
    return reinterpret_cast<void*>(1);
}
void destroy(void*){}
const char* name(void*){return "Independent Sound client test";}
}
bool obs_module_load(void){
    static obs_source_info info{};
    info.id="independent_sound_client_test";info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_CAP_DISABLED;info.get_name=name;info.create=create;info.destroy=destroy;
    obs_register_source(&info);return true;
}
