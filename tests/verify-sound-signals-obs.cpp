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
    if(argc!=8||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_set_ui_task_handler(queue_ui);
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 4;
    obs_module_t* m=nullptr;if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 5;
    auto* props=obs_get_source_properties("sound_tts_source");
    auto* modes=props?obs_properties_get(props,"speech_input"):nullptr;
    bool plugin=false;
    for(size_t i=0;modes&&i<obs_property_list_item_count(modes);++i)
        plugin|=std::string(obs_property_list_item_string(modes,i))=="plugin";
    if(props)obs_properties_destroy(props);
    printf("SOUND_SIGNAL_PLUGIN_INPUT=%d\n",plugin);
    if(!plugin){obs_shutdown();return 1;}
    obs_module_t* client_module=nullptr;
    if(obs_open_module(&client_module,argv[7],argv[2])!=MODULE_SUCCESS||!obs_init_module(client_module))return 13;
    auto* client=obs_source_create_private("independent_sound_client_test","Other plugin",nullptr);
    if(!client)return 14;
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
    auto* cfg2=obs_data_create();
    obs_data_set_string(cfg2,"backend","piper");
    auto* tts=obs_source_create("sound_tts_source","Signal destination",cfg2,nullptr);obs_data_release(cfg2);
    if(!tts)return 10;
    auto* ycfg=obs_data_create();obs_data_set_string(ycfg,"mediapipe_library",argv[5]);obs_data_set_string(ycfg,"yamnet_model",argv[6]);
    auto* yamnet=obs_source_create("yamnet_classifier_source","Sound events",ycfg,nullptr);obs_data_release(ycfg);
    if(!yamnet)return 11;
    obs_source_filter_add(mic,yamnet);
    // First ask the Sound provider for inventory; the separately loaded
    // consumer then uses the public client header against these channels.
    auto* inventory=obs_data_create();calldata_t args{};calldata_set_ptr(&args,"inventory",inventory);
    bool listed=proc_handler_call(obs_source_get_proc_handler(tts),"sound_signal_list_v1",&args)&&obs_data_get_int(inventory,"version")==1;
    calldata_free(&args);
    auto* rows=obs_data_get_array(inventory,"channels");
    std::string speech_key,tts_key,yamnet_key;
    for(size_t i=0;rows&&i<obs_data_array_count(rows);++i){
        auto* row=obs_data_array_item(rows,i);
        std::string kind=obs_data_get_string(row,"kind"),key=obs_data_get_string(row,"channel");
        if(kind=="transcript")speech_key=key;
        if(kind=="speech")tts_key=key;
        if(kind=="classification")yamnet_key=key;
        obs_data_release(row);
    }
    if(rows)obs_data_array_release(rows);obs_data_release(inventory);
    listed=listed&&!speech_key.empty()&&!tts_key.empty()&&!yamnet_key.empty();
    printf("SOUND_SIGNAL_INVENTORY=%d\n",listed);
    if(!listed)return 12;
    Speech audio;obs_source_add_audio_capture_callback(tts,capture,&audio);
    obs_set_output_source(2,tts);obs_source_set_audio_active(tts,true);
    std::this_thread::sleep_for(300ms);
    auto send=[&](const char* phrase){
        calldata_t d{};calldata_set_string(&d,"text",phrase);calldata_set_string(&d,"channel",tts_key.c_str());
        bool ok=proc_handler_call(obs_source_get_proc_handler(client),"probe_submit_v1",&d)&&calldata_bool(&d,"accepted");
        calldata_free(&d);return ok;
    };
    bool default_block=!send("This must not be spoken.");
    cfg2=obs_data_create();obs_data_set_string(cfg2,"speech_input","plugin");obs_source_update(tts,cfg2);obs_data_release(cfg2);
    feed(mic,speech);
    calldata_t interim{};calldata_set_string(&interim,"channel",speech_key.c_str());calldata_set_string(&interim,"stream","latest");
    bool partial=has_partial(stt)&&sequence(stt)==0&&proc_handler_call(obs_source_get_proc_handler(client),"sound_signal_read_v1",&interim)&&calldata_bool(&interim,"available")&&!calldata_bool(&interim,"final");
    calldata_free(&interim);
    feed(mic,std::vector<float>(96000,0));
    bool final=wait([&]{return sequence(stt)>=1;});
    calldata_t read{};calldata_set_string(&read,"channel",speech_key.c_str());calldata_set_string(&read,"stream","final");
    bool received=proc_handler_call(obs_source_get_proc_handler(client),"sound_signal_read_v1",&read)&&calldata_bool(&read,"available")&&calldata_bool(&read,"final");
    std::string transcript=calldata_string(&read,"text")?calldata_string(&read,"text"):"";calldata_clear(&read);
    calldata_set_string(&read,"channel",yamnet_key.c_str());calldata_set_string(&read,"stream","latest");
    bool classified=proc_handler_call(obs_source_get_proc_handler(tts),"sound_signal_read_v1",&read)&&calldata_bool(&read,"available")&&calldata_string(&read,"label")&&*calldata_string(&read,"label");
    calldata_clear(&read);
    calldata_set_string(&read,"channel",yamnet_key.c_str());calldata_set_string(&read,"stream","category");calldata_set_string(&read,"category","Speech");
    bool category=proc_handler_call(obs_source_get_proc_handler(client),"sound_signal_read_v1",&read)&&calldata_bool(&read,"available");
    calldata_clear(&read);
    bool accepted=received&&send(transcript.c_str());
    bool spoke=accepted&&wait([&]{return count(tts,"completed")==1;})&&audio.nonzero>10000;
    calldata_set_string(&read,"channel",tts_key.c_str());calldata_set_string(&read,"stream","latest");
    bool status=proc_handler_call(obs_source_get_proc_handler(client),"sound_signal_read_v1",&read)&&calldata_bool(&read,"available")&&calldata_int(&read,"completed")==1&&calldata_int(&read,"failed")==0;
    calldata_clear(&read);
    cfg2=obs_data_create();obs_data_set_string(cfg2,"backend","edge");obs_source_update(tts,cfg2);obs_data_release(cfg2);
    bool cloud_block=!send("Never send this to the cloud.");
    obs_source_set_enabled(tts,false);bool disabled=!send("Disabled.");
    obs_source_filter_remove(mic,stt);
    calldata_set_string(&read,"channel",speech_key.c_str());calldata_set_string(&read,"stream","final");
    bool removed=proc_handler_call(obs_source_get_proc_handler(client),"sound_signal_read_v1",&read)&&!calldata_bool(&read,"available");
    calldata_free(&read);
    bool ok=listed&&default_block&&partial&&final&&received&&classified&&category&&status&&spoke&&cloud_block&&disabled&&removed;
    printf("SOUND_SIGNAL_REAL list=%d default_block=%d interim=%d final=%d receive=%d classify=%d category=%d status=%d speak=%d cloud_block=%d disabled=%d removed=%d audio=%llu pass=%d\n",listed,default_block,partial,final,received,classified,category,status,spoke,cloud_block,disabled,removed,(unsigned long long)audio.nonzero.load(),ok);
    obs_source_filter_remove(mic,yamnet);obs_source_remove_audio_capture_callback(tts,capture,&audio);
    obs_set_output_source(2,nullptr);obs_set_output_source(1,nullptr);
    obs_source_release(client);obs_source_release(yamnet);obs_source_release(stt);obs_source_release(tts);obs_source_release(mic);obs_wait_for_destroy_queue();drain_ui();obs_shutdown();
    return !ok;
}
