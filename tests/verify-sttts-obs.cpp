#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdio>
#include <string>
#include <fstream>
#include <mutex>
using namespace std::chrono_literals;
static std::mutex ui_mutex;
static std::vector<std::pair<obs_task_t,void*>> ui_tasks;
static void queue_ui(obs_task_t task,void* ptr,bool wait){
    if(wait)std::abort();
    std::lock_guard lock(ui_mutex);ui_tasks.emplace_back(task,ptr);
}
static void drain_ui(){
    std::vector<std::pair<obs_task_t,void*>> tasks;
    {std::lock_guard lock(ui_mutex);tasks.swap(ui_tasks);}
    for(auto& task:tasks)task.first(task.second);
}

static const char* name(void*){return "Relay fixture";}
static void* create(obs_data_t*,obs_source_t*){return reinterpret_cast<void*>(1);}
static void destroy(void*){}
static obs_source_info input={.id="relay_fixture",.type=OBS_SOURCE_TYPE_INPUT,.output_flags=OBS_SOURCE_AUDIO,.get_name=name,.create=create,.destroy=destroy};
struct Speech {std::atomic<uint64_t> nonzero{0};};
static void capture(void* ptr,obs_source_t*,const audio_data* a,bool){
    if(!a->data[0])return;
    auto* p=reinterpret_cast<const float*>(a->data[0]);uint64_t count=0;
    for(unsigned i=0;i<a->frames;++i)if(std::abs(p[i])>.001f)++count;
    static_cast<Speech*>(ptr)->nonzero+=count;
}
static int64_t count(obs_source_t* s,const char* key){
    calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_tts_status_v1",&d);
    auto v=ok?calldata_int(&d,key):-1;calldata_free(&d);return v;
}
static int64_t sequence(obs_source_t* f){
    calldata_t d{};proc_handler_call(obs_source_get_proc_handler(f),"sound_stt_read_v1",&d);
    auto v=calldata_int(&d,"sequence");calldata_free(&d);return v;
}
static bool ready(obs_source_t* f){
    calldata_t d{};proc_handler_call(obs_source_get_proc_handler(f),"sound_stt_read_v1",&d);
    auto* status=calldata_string(&d,"status");bool result=status&&std::string(status)=="ready";
    calldata_free(&d);return result;
}
template<class F>static bool wait(F fn,int sec=20){
    for(int i=0;i<sec*20;++i){drain_ui();if(fn())return true;std::this_thread::sleep_for(50ms);}return false;
}
static bool has_partial(obs_source_t* f){
    calldata_t d{};proc_handler_call(obs_source_get_proc_handler(f),"sound_stt_read_v2",&d);
    bool result=calldata_bool(&d,"available")&&!calldata_bool(&d,"final");calldata_free(&d);return result;
}
static void feed(obs_source_t* source,const std::vector<float>& pcm){
    constexpr unsigned rate=48000,block=480;
    std::vector<float> samples(block);auto start=os_gettime_ns();auto clock=std::chrono::steady_clock::now();
    for(size_t pos=0;pos<pcm.size();pos+=block){
        for(size_t i=0;i<block;++i)samples[i]=pos+i<pcm.size()?pcm[pos+i]:0.f;
        obs_source_audio a{};a.data[0]=a.data[1]=reinterpret_cast<const uint8_t*>(samples.data());
        a.frames=block;a.speakers=SPEAKERS_STEREO;a.format=AUDIO_FORMAT_FLOAT_PLANAR;a.samples_per_sec=rate;a.timestamp=start+pos*1000000000ULL/rate;
        obs_source_output_audio(source,&a);
        std::this_thread::sleep_until(clock+std::chrono::nanoseconds((pos+block)*1000000000ULL/rate));drain_ui();
    }
}
int main(int argc,char** argv){
    if(argc!=5||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_set_ui_task_handler(queue_ui);
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_module_t* m=nullptr;if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 5;
    auto* props=obs_get_source_properties("sound_tts_source");
    bool ui=props&&obs_properties_get(props,"speech_input")&&obs_properties_get(props,"stt_source");
    if(props)obs_properties_destroy(props);
    printf("SOUND_STTTS_PICKER=%d\n",ui);
    if(!ui){obs_shutdown();return 1;}
    std::ifstream file(argv[4],std::ios::binary|std::ios::ate);auto n=file.tellg();if(n<=0||n%4)return 6;
    std::vector<float> speech(size_t(n)/4);file.seekg(0);file.read(reinterpret_cast<char*>(speech.data()),n);
    // Short spoken excerpt from the actual Nemotron fixture, followed by silence.
    speech.resize(std::min<size_t>(speech.size(),48000*4));
    auto utterance=speech;utterance.resize(utterance.size()+48000*2,0);
    obs_register_source(&input);
    auto* mic=obs_source_create("relay_fixture","Relay mic",nullptr,nullptr);
    auto* cfg=obs_data_create();obs_data_set_string(cfg,"backend","nemotron");
    obs_data_set_string(cfg,"nemotron_model_path",argv[3]);obs_data_set_int(cfg,"silence_ms",900);obs_data_set_int(cfg,"caption_hold_ms",15000);
    auto* stt=obs_source_create("sound_stt_filter","Relay STT",cfg,nullptr);obs_data_release(cfg);
    if(!mic||!stt)return 7;
    obs_source_filter_add(mic,stt);obs_set_output_source(1,mic);obs_source_set_audio_active(mic,true);
    if(!wait([&]{return ready(stt);}))return 8;
    feed(mic,utterance);if(!wait([&]{return sequence(stt)>=1;}))return 9;
    auto baseline=sequence(stt);
    cfg=obs_data_create();obs_data_set_string(cfg,"backend","piper");obs_data_set_string(cfg,"speech_input","stt");
    std::string key=std::string(obs_source_get_uuid(mic))+"/"+obs_source_get_uuid(stt);
    obs_data_set_string(cfg,"stt_source",key.c_str());
    auto* tts=obs_source_create("sound_tts_source","Speech relay",cfg,nullptr);obs_data_release(cfg);
    if(!tts)return 10;
    props=obs_source_properties(tts);auto* list=obs_properties_get(props,"stt_source");bool discovered=false;
    for(size_t i=0;list&&i<obs_property_list_item_count(list);++i)discovered|=key==obs_property_list_item_string(list,i);
    bool visible=list&&obs_property_visible(list)&&!obs_property_visible(obs_properties_get(props,"monitor_text"));
    obs_properties_destroy(props);
    Speech audio;obs_source_add_audio_capture_callback(tts,capture,&audio);obs_set_output_source(2,tts);obs_source_set_audio_active(tts,true);
    std::this_thread::sleep_for(500ms);bool no_stale=count(tts,"completed")==0&&audio.nonzero==0;
    feed(mic,speech);bool interim=has_partial(stt)&&sequence(stt)==baseline&&audio.nonzero==0;
    feed(mic,std::vector<float>(96000,0));
    bool one=wait([&]{return count(tts,"completed")==1;})&&audio.nonzero>10000;
    obs_source_set_name(mic,"Renamed mic");obs_source_set_name(stt,"Renamed STT");
    feed(mic,utterance);
    bool twice=wait([&]{return count(tts,"completed")==2;});
    std::this_thread::sleep_for(800ms);bool dedup=count(tts,"completed")==2&&count(tts,"failed")==0;
    obs_source_set_enabled(tts,false);feed(mic,utterance);
    obs_source_set_enabled(tts,true);std::this_thread::sleep_for(700ms);
    bool no_replay=count(tts,"completed")==2;
    // Explicit cloud permission defaults off. This must consume the new
    // final without starting synthesis, even if Edge happened to be selected.
    cfg=obs_data_create();obs_data_set_string(cfg,"backend","edge");obs_source_update(tts,cfg);obs_data_release(cfg);
    auto before_cloud=sequence(stt);auto before_audio=audio.nonzero.load();
    feed(mic,utterance);bool cloud_block=wait([&]{return sequence(stt)>before_cloud;});
    std::this_thread::sleep_for(1200ms);
    cloud_block=cloud_block&&count(tts,"completed")==2&&count(tts,"failed")==0&&audio.nonzero==before_audio;
    obs_source_filter_remove(mic,stt);std::this_thread::sleep_for(200ms);
    // A TTS source cannot be a selectable producer even if it has an STT filter.
    obs_source_filter_add(tts,stt);
    props=obs_source_properties(tts);list=obs_properties_get(props,"stt_source");
    std::string loop=std::string(obs_source_get_uuid(tts))+"/"+obs_source_get_uuid(stt);
    bool no_loop=true;for(size_t i=0;list&&i<obs_property_list_item_count(list);++i)no_loop&=loop!=obs_property_list_item_string(list,i);
    obs_properties_destroy(props);
    obs_source_filter_remove(tts,stt);
    bool ok=discovered&&visible&&no_stale&&interim&&one&&twice&&dedup&&no_replay&&no_loop&&cloud_block;
    printf("SOUND_STTTS_REAL discover=%d visible=%d stale=%d partial=%d first=%d repeat=%d dedup=%d resume=%d loop=%d cloud_block=%d audio=%llu pass=%d\n",discovered,visible,no_stale,interim,one,twice,dedup,no_replay,no_loop,cloud_block,(unsigned long long)audio.nonzero.load(),ok);
    obs_source_remove_audio_capture_callback(tts,capture,&audio);obs_set_output_source(2,nullptr);obs_set_output_source(1,nullptr);
    obs_source_release(stt);obs_source_release(tts);obs_source_release(mic);obs_wait_for_destroy_queue();drain_ui();obs_shutdown();
    return !ok;
}
