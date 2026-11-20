// Opt-in, bounded delayed-live OBS filter backed by the user's existing
// local HTDemucs sidecar. Its real-time callback never does model inference,
// socket I/O, mutex locking, heap allocation or filesystem operations.
#include <obs-module.h>
#include <media-io/audio-io.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "framework/live-stem-pipeline.hpp"
#include "framework/live-stem-client.hpp"

namespace {
struct StemFilter {
    explicit StemFilter(uint32_t rate)
        : sample_rate(rate),pipeline(rate),client(stopping) {}
    uint32_t sample_rate;
    stts::LiveStemPipeline pipeline;
    std::atomic<bool> stopping{false};
    stts::LiveStemClient client;
    std::atomic<int> selected{3}; // drums=0 bass=1 other=2 vocals=3
    std::string socket_path;
    std::thread worker;
};

const char* get_name(void*) {
    return "Live Latent: Buffered stem (experimental)";
}
void defaults(obs_data_t* settings) {
    obs_data_set_default_int(settings,"stem",3);
    const char* dir=std::getenv("XDG_RUNTIME_DIR");
    if(dir && *dir){
        const std::string path=std::string(dir)+"/lll-sound-stems.sock";
        obs_data_set_default_string(settings,"socket_path",path.c_str());
    }
}
void update(void* ptr,obs_data_t* settings) {
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
    int selected=int(obs_data_get_int(settings,"stem"));
    if(selected<0 || selected>3)selected=3;
    data->selected.store(selected,std::memory_order_release);
    // Socket is intentionally immutable during the session; restart this
    // filter for a different sidecar instead of tearing down an audio thread.
}
obs_properties_t* properties(void*) {
    auto* props=obs_properties_create();
    obs_properties_add_text(props,"socket_path",
        "Local worker socket (requires recreating filter to change)",OBS_TEXT_DEFAULT);
    auto* list=obs_properties_add_list(props,"stem","Separated stereo stem",
        OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(list,"Vocals",3);
    obs_property_list_add_int(list,"Drums",0);
    obs_property_list_add_int(list,"Bass",1);
    obs_property_list_add_int(list,"Other",2);
    obs_properties_add_text(props,"delay_note",
        "Fixed 12 s delay; first 12 s silent. Needs local 8 s HTDemucs worker. "
        "One selected stereo stem per filter. A missed deadline mutes the filter "
        "until recreated (never plays dry music as a stem).",OBS_TEXT_INFO);
    return props;
}

void worker_loop(StemFilter* data) {
    const auto frames=data->pipeline.window_frames();
    std::vector<float> input_l(frames),input_r(frames);
    std::vector<float> output_l(frames),output_r(frames);
    uint32_t seq=0;
    while(!data->stopping.load(std::memory_order_acquire)){
        if(!data->pipeline.ok())break;
        if(!data->pipeline.read_input_window(input_l.data(),input_r.data())){
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        const int stem=data->selected.load(std::memory_order_acquire);
        if(!data->client.exchange(data->socket_path,data->sample_rate,seq++,
                input_l.data(),input_r.data(),frames,stem,
                output_l.data(),output_r.data())){
            if(!data->stopping.load())
                blog(LOG_WARNING,"[Sound stems] Local sidecar request failed; "
                     "muting until this filter is recreated");
            data->pipeline.invalidate();
            break;
        }
        if(!data->pipeline.push_output_window(output_l.data(),output_r.data())){
            data->pipeline.invalidate();
            blog(LOG_WARNING,"[Sound stems] Output queue full; muted to prevent desync");
            break;
        }
    }
}

void* create(obs_data_t* settings,obs_source_t*) {
    auto* audio=obs_get_audio();
    const uint32_t rate=audio?audio_output_get_sample_rate(audio):0;
    if(rate!=48000 && rate!=44100){
        blog(LOG_WARNING,"[Sound stems] Requires OBS stereo project at 44.1 or 48 kHz");
        return nullptr;
    }
    auto* data=new(std::nothrow) StemFilter(rate);
    if(!data)return nullptr;
    const char* path=obs_data_get_string(settings,"socket_path");
    if(path)data->socket_path=path;
    if(data->socket_path.empty() || data->socket_path.size()>104){
        blog(LOG_WARNING,"[Sound stems] Missing/invalid local worker socket path");
        delete data;return nullptr;
    }
    update(data,settings);
    try {
        data->worker=std::thread(worker_loop,data);
    } catch(...) {
        delete data;return nullptr;
    }
    blog(LOG_INFO,"[Sound stems] Experimental stereo delayed filter created "
         "(rate=%u, delay=12s, selected=%d, worker=%s)",
         rate,data->selected.load(),data->socket_path.c_str());
    return data;
}
void destroy(void* ptr) {
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data)return;
    data->stopping.store(true,std::memory_order_release);
    data->client.cancel(); // interrupt the worker, never an OBS audio call
    if(data->worker.joinable())data->worker.join();
    delete data;
}
obs_audio_data* filter_audio(void* ptr,obs_audio_data* packet){
    auto* data=static_cast<StemFilter*>(ptr);
    if(!data || !packet)return packet;
    auto zero=[&]{
        for(size_t ch=0;ch<MAX_AUDIO_CHANNELS;++ch)
            if(packet->data[ch])
                std::fill_n(reinterpret_cast<float*>(packet->data[ch]),
                            packet->frames,0.f);
    };
    if(!packet->frames)return packet;
    if(!packet->data[0] || !packet->data[1] || !data->pipeline.ok()){
        data->pipeline.invalidate();
        zero();
        return packet;
    }
    auto* left=reinterpret_cast<float*>(packet->data[0]);
    auto* right=reinterpret_cast<float*>(packet->data[1]);
    data->pipeline.process(left,right,packet->frames);
    for(size_t ch=2;ch<MAX_AUDIO_CHANNELS;++ch)
        if(packet->data[ch])
            std::fill_n(reinterpret_cast<float*>(packet->data[ch]),packet->frames,0.f);
    return packet;
}
} // namespace

extern "C" {
struct obs_source_info stem_live_filter={
    .id="stem_live_buffered_filter",
    .type=OBS_SOURCE_TYPE_FILTER,
    .output_flags=OBS_SOURCE_AUDIO,
    .get_name=get_name,
    .create=create,
    .destroy=destroy,
    .get_defaults=defaults,
    .get_properties=properties,
    .update=update,
    .filter_audio=filter_audio,
};
}
