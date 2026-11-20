// Real speech via the production OBS source, actual backend and audio capture.
#include <obs.h>
#include <obs-module.h>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <cstdlib>
using namespace std::chrono_literals;
struct Capture {
    std::atomic<uint64_t> frames{0},nonzero{0},bad{0},first{0},last{0};
};
static void capture(void* ptr,obs_source_t*,const audio_data* data,bool){
    auto& s=*static_cast<Capture*>(ptr);
    const auto* p=reinterpret_cast<const float*>(data->data[0]);
    if(!p)return;
    uint64_t nz=0;
    for(unsigned i=0;i<data->frames;++i){
        if(!std::isfinite(p[i]))++s.bad;
        if(std::abs(p[i])>.001f)++nz;
    }
    if(!s.first.load())s.first=data->timestamp;
    if(s.last.load()>data->timestamp)++s.bad;
    s.last=data->timestamp;s.frames+=data->frames;s.nonzero+=nz;
}
static int64_t count(obs_source_t* s,const char* key){
    calldata_t args{};const bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_tts_status_v1",&args);
    int64_t n=ok?calldata_int(&args,key):-1;calldata_free(&args);return n;
}
static bool wait_count(obs_source_t* s,int expected,int seconds=25){
    for(int i=0;i<seconds*20;++i){
        if(count(s,"failed")!=0)return false;
        if(count(s,"completed")==expected)return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}
static void button(obs_source_t* s,const char* key){
    auto* p=obs_source_properties(s);auto* b=obs_properties_get(p,key);
    if(b)obs_property_button_clicked(b,s);
    obs_properties_destroy(p);
}
int main(int argc,char** argv){
    if(argc!=5)return 2;
    if(!obs_startup("en-US",nullptr,nullptr))return 3;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 4;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;
    v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;
    v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 5;
    obs_module_t* module=nullptr;
    if(obs_open_module(&module,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(module))return 6;
    const std::string backend=argv[3];const bool long_text=backend=="long";const bool options_test=backend=="options";const bool unified=backend=="unified"||long_text||options_test;
    const bool monitoring=backend=="piper";
    obs_source_t* text=nullptr;
    if(monitoring){
        obs_module_t* tm=nullptr;
        if(obs_open_module(&tm,"/usr/local/lib/obs-plugins/text-freetype2.so",
            "/usr/local/share/obs/obs-plugins/text-freetype2")!=MODULE_SUCCESS||!obs_init_module(tm))return 7;
        auto* cfg=obs_data_create();obs_data_set_string(cfg,"text","Hello from Live Latent Lab.");
        text=obs_source_create("text_ft2_source","Speech text fixture",cfg,nullptr);obs_data_release(cfg);
        if(!text)return 8;
    }
    auto* cfg=obs_data_create();
    obs_data_set_string(cfg,"text","Hello from Live Latent Lab.");
    if(text){obs_data_set_string(cfg,"text_source",obs_source_get_uuid(text));obs_data_set_bool(cfg,"monitor_text",true);}
    if(unified){
        obs_data_set_string(cfg,"backend","piper");
        obs_data_set_string(cfg,"voice_kokoro","am_michael");
        obs_data_set_string(cfg,"voice_tiktok","en_us_stitch");
    }
    if(!unified){
        obs_data_set_string(cfg,"backend",backend.c_str());
        obs_data_set_string(cfg,("voice_"+backend).c_str(),argv[4]);
    }
    auto* source=obs_source_create("sound_tts_source","Speech generator",cfg,nullptr);
    obs_data_release(cfg);
    if(!source)return 9;
    if(options_test){
        const char* engines[]={"kokoro","piper","chatterbox","edge","tiktok"};
        const char* representative[]={"kokoro_speed","piper_length_scale","chatterbox_temperature","edge_rate","tiktok_controls_note"};
        auto* settings=obs_source_get_settings(source);
        auto* props=obs_source_properties(source);
        bool picker=true;
        for(int i=0;i<5;++i){
            obs_data_set_string(settings,"backend",engines[i]);
            obs_property_modified(obs_properties_get(props,"backend"),settings);
            for(int j=0;j<5;++j){
                auto* field=obs_properties_get(props,representative[j]);
                picker=picker&&field&&obs_property_visible(field)==(i==j);
            }
        }
        auto* ignored=obs_properties_get(props,"chatterbox_exaggeration");
        picker=picker&&!ignored;
        picker=picker&&!obs_properties_get(props,"kokoro_continuous")&&!obs_properties_get(props,"piper_speaker_id");
        obs_data_set_string(settings,"backend","kokoro");
        obs_property_modified(obs_properties_get(props,"backend"),settings);
        auto* phonemes=obs_properties_get(props,"kokoro_is_phonemes");
        auto* language=obs_properties_get(props,"kokoro_language");
        picker=picker&&phonemes&&language;
        if(phonemes&&language){
            obs_data_set_bool(settings,"kokoro_is_phonemes",true);
            obs_property_modified(phonemes,settings);
            picker=picker&&!obs_property_visible(language);
            obs_data_set_bool(settings,"kokoro_is_phonemes",false);
            obs_property_modified(phonemes,settings);
            picker=picker&&obs_property_visible(language);
        }

        obs_properties_destroy(props);obs_data_release(settings);
        std::printf("SOUND_TTS_OPTIONS_PICKER=%d\n",picker);
        if(!picker){obs_source_release(source);obs_wait_for_destroy_queue();obs_shutdown();return 1;}
        Capture audio;
        obs_source_add_audio_capture_callback(source,capture,&audio);
        obs_set_output_source(1,source);obs_source_set_audio_mixers(source,1);
        obs_source_set_audio_active(source,true);
        std::this_thread::sleep_for(350ms);
        bool ok=true;
        uint64_t lengths[6]={};
        const char* sequence[]={"kokoro","kokoro","piper","piper","edge","edge"};
        for(int i=0;i<6&&ok;++i){
            auto* d=obs_source_get_settings(source);
            obs_data_set_string(d,"backend",sequence[i]);
            obs_data_set_double(d,"kokoro_speed",1.);
            obs_data_set_double(d,"piper_length_scale",1.);
            obs_data_set_int(d,"edge_rate",0);
            if(i==1)obs_data_set_double(d,"kokoro_speed",1.6);
            if(i==3)obs_data_set_double(d,"piper_length_scale",1.8);
            if(i==5){obs_data_set_int(d,"edge_rate",60);obs_data_set_int(d,"edge_pitch",15);obs_data_set_int(d,"edge_volume",-20);}
            obs_source_update(source,d);obs_data_release(d);
            auto before=audio.frames.load();
            button(source,"speak");
            ok=wait_count(source,i+1,35);
            lengths[i]=audio.frames.load()-before;
            std::printf("SOUND_TTS_OPTIONS_PLAY engine=%s trial=%d frames=%llu pass=%d\n",sequence[i],i,(unsigned long long)lengths[i],ok);
        }
        ok=ok&&lengths[1]<lengths[0]*.85&&lengths[3]>lengths[2]*1.25&&lengths[5]<lengths[4]*.85;
        // Verify that switching back restores the independent settings,
        // with the exact same source identity and no re-creation.
        auto* d=obs_source_get_settings(source);
        obs_data_set_double(d,"kokoro_speed",1.35);
        obs_data_set_string(d,"backend","chatterbox");
        obs_data_set_double(d,"chatterbox_temperature",.65);
        obs_data_set_int(d,"chatterbox_top_k",80);
        obs_data_set_double(d,"chatterbox_top_p",.85);
        obs_data_set_double(d,"chatterbox_repetition_penalty",1.1);
        obs_source_update(source,d);obs_data_release(d);
        button(source,"speak");
        ok=ok&&wait_count(source,7,35);
        if(const auto* reference=std::getenv("SOUND_TTS_TEST_REFERENCE")){
            d=obs_source_get_settings(source);
            obs_data_set_string(d,"chatterbox_audio_prompt_path",reference);
            obs_data_set_bool(d,"chatterbox_norm_loudness",false);
            obs_source_update(source,d);obs_data_release(d);
            button(source,"speak");ok=ok&&wait_count(source,8,35);
            d=obs_source_get_settings(source);
            obs_data_set_string(d,"chatterbox_audio_prompt_path","");
            obs_source_update(source,d);obs_data_release(d);
            button(source,"speak");ok=ok&&wait_count(source,9,35);
            std::printf("SOUND_TTS_OPTIONS_REFERENCE_PASS=%d\n",ok);
        }else ok=false;

        d=obs_source_get_settings(source);
        obs_data_set_string(d,"backend","kokoro");obs_source_update(source,d);
        ok=ok&&obs_data_get_double(d,"kokoro_speed")==1.35&&obs_data_get_int(d,"edge_rate")==60;
        obs_data_release(d);
        std::printf("SOUND_TTS_OPTIONS_OBS_PASS=%d\n",ok);
        obs_source_remove_audio_capture_callback(source,capture,&audio);
        obs_set_output_source(1,nullptr);obs_source_release(source);obs_wait_for_destroy_queue();obs_shutdown();
        return ok?0:1;
    }
    if(long_text){
        // Reproduce the desktop scene path and the user's 673-byte input
        // using neutral synthetic text, not the user's actual content.
        auto* scene=obs_scene_create("Long speech scene");
        auto* item=obs_scene_add(scene,source);
        obs_set_output_source(0,obs_scene_get_source(scene));
        Capture audio;
        obs_source_add_audio_capture_callback(source,capture,&audio);
        obs_source_set_audio_mixers(source,1);
        std::string sample;
        while(sample.size()<673)sample+="The quick brown fox jumps over the lazy dog. ";
        sample.resize(673);
        auto* settings=obs_source_get_settings(source);
        obs_data_set_string(settings,"text",sample.c_str());
        obs_source_update(source,settings);obs_data_release(settings);
        std::this_thread::sleep_for(500ms);
        button(source,"speak");
        bool started=false;
        for(int i=0;i<200;++i){
            if(audio.nonzero.load()>1000){started=true;break;}
            std::this_thread::sleep_for(50ms);
        }
        bool played=started&&wait_count(source,1,100);
        const bool ok=played&&audio.bad.load()==0&&audio.nonzero.load()>48000*5;
        std::printf("SOUND_TTS_LONG_OBS bytes=%zu started=%d completed=%d frames=%llu pass=%d\n",sample.size(),started,played,(unsigned long long)audio.frames.load(),ok);
        obs_source_remove_audio_capture_callback(source,capture,&audio);
        obs_set_output_source(0,nullptr);obs_sceneitem_remove(item);obs_scene_release(scene);
        obs_source_release(source);obs_wait_for_destroy_queue();obs_shutdown();return ok?0:1;
    }
    if(unified){
        Capture audio;
        obs_source_add_audio_capture_callback(source,capture,&audio);
        obs_set_output_source(1,source);obs_source_set_audio_mixers(source,1);
        obs_source_set_audio_active(source,true);
        std::this_thread::sleep_for(350ms);
        const char* engines[]={"piper","kokoro","chatterbox","tiktok","edge","kokoro","edge"};
        const char* expected[]={"en_US-amy-low","am_michael","default","en_us_stitch","en-US-AriaNeural","am_michael","en-US-AriaNeural"};
        bool ok=true;
        {auto* props=obs_source_properties(source);
         auto* edge=obs_properties_get(props,"voice_edge");
         ok=edge&&obs_property_list_item_count(edge)==318;
         obs_properties_destroy(props);}
        for(int i=0;i<7&&ok;++i){
            auto* settings=obs_source_get_settings(source);
            obs_data_set_string(settings,"backend",engines[i]);
            auto* props=obs_source_properties(source);
            auto* selector=obs_properties_get(props,"backend");
            if(!selector||!obs_property_modified(selector,settings)){ok=false;}
            for(int j=0;j<5&&ok;++j){
                auto key=std::string("voice_")+engines[j];
                auto* voice=obs_properties_get(props,key.c_str());
                bool current=std::string(engines[j])==engines[i];
                if(!voice||obs_property_visible(voice)!=current)ok=false;
                if(current&&std::string(obs_data_get_string(settings,key.c_str()))!=expected[i])ok=false;
            }
            if(std::string(engines[i])=="edge"){
                auto* v=obs_properties_get(props,"voice_edge");
                bool found=false;
                if(v)for(size_t k=0;k<obs_property_list_item_count(v);++k)
                    found|=std::string(obs_property_list_item_string(v,k))==expected[i];
                ok=ok&&found&&obs_property_list_item_count(v)==318;
            }
            auto* cloud=obs_properties_get(props,"cloud_note");
            ok=ok&&cloud&&obs_property_visible(cloud)==((std::string(engines[i])=="tiktok"||std::string(engines[i])=="edge"));
            obs_properties_destroy(props);
            obs_source_update(source,settings);obs_data_release(settings);
            if(!ok)break;
            auto before=audio.nonzero.load();
            button(source,"speak");
            bool played=wait_count(source,i+1,35)&&audio.nonzero.load()>before+1000;
            std::printf("SOUND_TTS_UNIFIED_SWITCH backend=%s index=%d voice=%s pass=%d\n",engines[i],i,expected[i],int(ok&&played));
            ok=ok&&played;
        }
        // An in-flight long utterance must not complete after switching backend.
        if(ok){
            auto* settings=obs_source_get_settings(source);
            obs_data_set_string(settings,"backend","piper");
            obs_data_set_string(settings,"text","A longer sentence intended to be interrupted by a backend change, with enough words to keep the playout active for several seconds.");
            obs_source_update(source,settings);obs_data_release(settings);
            auto before=audio.frames.load();button(source,"speak");
            for(int i=0;i<200&&audio.frames.load()==before;++i)std::this_thread::sleep_for(50ms);
            const bool started=audio.frames.load()>before;
            settings=obs_source_get_settings(source);
            obs_data_set_string(settings,"backend","kokoro");
            obs_data_set_string(settings,"text","Backend switched.");
            obs_source_update(source,settings);obs_data_release(settings);
            std::this_thread::sleep_for(250ms);
            before=audio.frames.load();std::this_thread::sleep_for(400ms);
            bool canceled=started&&audio.frames.load()==before&&count(source,"completed")==7;
            button(source,"speak");
            bool recovered=wait_count(source,8,35);
            ok=canceled&&recovered&&audio.bad.load()==0;
            std::printf("SOUND_TTS_UNIFIED_CANCEL canceled=%d recovered=%d\n",canceled,recovered);
        }
        std::printf("SOUND_TTS_UNIFIED_PASS=%d\n",ok);
        obs_source_remove_audio_capture_callback(source,capture,&audio);
        obs_set_output_source(1,nullptr);obs_source_release(source);obs_shutdown();
        return ok?0:1;
    }
    auto* props=obs_source_properties(source);
    auto* voices=obs_properties_get(props,("voice_"+backend).c_str());
    auto* texts=obs_properties_get(props,"text_source");
    bool picker=false;
    for(size_t i=0;voices&&i<obs_property_list_item_count(voices);++i)
        picker|=std::string(obs_property_list_item_string(voices,i))==argv[4];
    bool text_picker=!text;
    for(size_t i=0;texts&&text&&i<obs_property_list_item_count(texts);++i)
        text_picker|=std::string(obs_property_list_item_string(texts,i))==obs_source_get_uuid(text);
    obs_properties_destroy(props);
    Capture audio;
    obs_source_add_audio_capture_callback(source,capture,&audio);
    obs_set_output_source(1,source);obs_source_set_audio_mixers(source,1);
    obs_source_set_audio_active(source,true);
    std::this_thread::sleep_for(350ms);
    if(!monitoring)button(source,"speak");
    const bool first=wait_count(source,1,35);
    const uint64_t frames=audio.frames.load(),nz=audio.nonzero.load();
    const double clock_sec=(audio.last.load()-audio.first.load())/1e9;
    bool dedup=first,second=first,cancel=true;
    if(monitoring&&first){
        // unchanged producer text must not trigger another utterance
        std::this_thread::sleep_for(1200ms);
        dedup=count(source,"completed")==1;
        auto* change=obs_data_create();obs_data_set_string(change,"text","A second message.");obs_source_update(text,change);obs_data_release(change);
        second=wait_count(source,2,15);
        // Disabling the source cancels in-flight/queued speech.
        change=obs_data_create();obs_data_set_string(change,"text","This is a much longer sentence, intended to test cancellation while speech is running.");obs_source_update(text,change);obs_data_release(change);
        std::this_thread::sleep_for(1000ms);
        obs_source_set_enabled(source,false);
        std::this_thread::sleep_for(350ms);
        auto before=audio.frames.load();
        std::this_thread::sleep_for(600ms);
        cancel=count(source,"completed")==2&&audio.frames.load()==before;
    }
    const bool ok=picker&&text_picker&&first&&frames>20000&&nz>1000&&audio.bad.load()==0&&clock_sec>.4&&dedup&&second&&cancel;
    std::printf("SOUND_TTS_REAL_OBS backend=%s picker=%d text_picker=%d first=%d frames=%llu nonzero=%llu clock_seconds=%.3f bad=%llu dedup=%d second=%d cancel=%d completed=%lld failed=%lld pass=%d\n",
        backend.c_str(),picker,text_picker,first,(unsigned long long)frames,(unsigned long long)nz,clock_sec,(unsigned long long)audio.bad.load(),
        dedup,second,cancel,(long long)count(source,"completed"),(long long)count(source,"failed"),ok);
    obs_source_remove_audio_capture_callback(source,capture,&audio);
    obs_set_output_source(1,nullptr);
    obs_source_release(source);
    if(text)obs_source_release(text);
    obs_shutdown();return ok?0:1;
}
