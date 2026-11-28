#include <obs.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/bmem.h>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed: %s\n",#x);return 1;}}while(false)
namespace fs=std::filesystem;
int main(int argc,char**argv){
 CHECK(argc==4||argc==5||argc==6);
 const bool fresh=argc==5&&(std::string(argv[4])=="--fresh"||std::string(argv[4])=="--fresh-expanded");
 const bool expanded=(argc==5&&(std::string(argv[4])=="--expanded"||std::string(argv[4])=="--fresh-expanded"))||(argc==6&&std::string(argv[4])=="--expanded-read-only");
 const bool installed=argc==6&&(std::string(argv[4])=="--read-only"||std::string(argv[4])=="--expanded-read-only");
 fs::path base=argv[3],models=installed?fs::path(argv[5]):base/"model library, test",runtime=base/"runtime library";
 if(!installed&&!fresh){
     fs::create_directories(models);fs::create_directories(runtime);
     for(auto sub:{"yamnet","STT/Whisper","STT/Zipformer","STT/Nemotron","STT/Parakeet","ddsp-vst-2022","RVC-GGUF","RVC-GGUF/voices","sa3-gguf","Mel-Band-RoFormer-GGUF","Demucs","Mel-Band-RoFormer"})
         fs::create_directories(models/sub);
     fs::create_directories(base/"external voices");
 }
 CHECK(obs_startup("en-US",base.string().c_str(),nullptr));
 obs_module_t*m=nullptr;CHECK(obs_open_module(&m,argv[1],argv[2])==MODULE_SUCCESS&&obs_init_module(m));
 char* path=obs_module_get_config_path(m,"preferences.ini");CHECK(path);
 if(fresh){CHECK(!fs::exists(path));models=fs::path(path).parent_path()/"models";}
 if(!installed&&!fresh)fs::create_directories(fs::path(path).parent_path());
 config_t* c=nullptr;
 if(!installed&&!fresh){CHECK(config_open(&c,path,CONFIG_OPEN_ALWAYS)==CONFIG_SUCCESS);
 config_set_string(c,"General","models",models.string().c_str());
 config_set_string(c,"General","runtimes",runtime.string().c_str());
 config_set_string(c,"ModelDirectories","rvc_voices",(base/"external voices").string().c_str());
 CHECK(config_save(c)==CONFIG_SUCCESS);config_close(c);}
 auto expected_dir=[&](const char* family,const char* relative){
     if(installed){
         config_t* actual=nullptr;
         if(config_open(&actual,path,CONFIG_OPEN_EXISTING)==CONFIG_SUCCESS){
             const char* value=config_get_string(actual,"ModelDirectories",family);
             fs::path chosen=value?value:"";config_close(actual);
             if(chosen.is_absolute()&&fs::is_directory(chosen))return chosen;
         }
     }
     return fresh?models:models/relative;
 };
 auto check=[&](const char* id,const char* key,const fs::path& expected){
     auto* p=obs_get_source_properties(id);
     if(!p)return false;
     auto* field=obs_properties_get(p,key);
     auto* value=field?obs_property_path_default_path(field):nullptr;
     bool ok=value&&expected.string()==value&&(!fresh||expected.empty()||fs::is_directory(expected));
     printf("PICKER id=%s key=%s path=%s pass=%d\n",id,key,value?value:"(null)",ok);
     obs_properties_destroy(p);return ok;
 };
 CHECK(check("yamnet_classifier_source","yamnet_model",fresh?models:models/"yamnet"));
 if(!installed)CHECK(check("yamnet_classifier_source","mediapipe_library",fresh?fs::path{}:runtime));
 if(expanded){
   if(fresh){
       auto* ddsp=obs_get_source_defaults("sound_ddsp_filter");
       auto* rvc=obs_get_source_defaults("stts_rvc_filter");
       CHECK(ddsp&&rvc);
       CHECK(!std::strlen(obs_data_get_string(ddsp,"models")));
       CHECK(!std::strlen(obs_data_get_string(rvc,"model")));
       obs_data_release(ddsp);obs_data_release(rvc);
   }
   CHECK(check("sound_stt_filter","model_path",expected_dir("whisper","STT/Whisper")));
   CHECK(check("sound_stt_filter","zipformer_model_dir",expected_dir("zipformer","STT/Zipformer")));
   CHECK(check("sound_stt_filter","nemotron_model_path",expected_dir("nemotron","STT/Nemotron")));
   CHECK(check("sound_stt_filter","parakeet_model_path",expected_dir("parakeet","STT/Parakeet")));
   CHECK(check("sound_ddsp_filter","models",(fresh?models:models/"ddsp-vst-2022")));
   CHECK(check("stts_rvc_filter","model",(fresh?models:models/"RVC-GGUF")));
   CHECK(check("stts_rvc_filter","voice_checkpoint",((installed||fresh)?expected_dir("rvc_voices","RVC-GGUF/voices"):base/"external voices")));
   CHECK(check("stts_rvc_filter","retrieval_index",((installed||fresh)?expected_dir("rvc_voices","RVC-GGUF/voices"):base/"external voices")));
   CHECK(check("sound_audio_diffusion_source","models_dir",(fresh?models:models/"sa3-gguf")));
   CHECK(check("sound_audio_diffusion_source","lora_a",(fresh?models:models/"sa3-gguf")));
   CHECK(check("sound_audio_diffusion_source","lora_b",(fresh?models:models/"sa3-gguf")));
   CHECK(check("stem_live_melband_filter","native_model",(fresh?models:models/"Mel-Band-RoFormer-GGUF")));
   CHECK(check("stem_live_melband_filter","worker_model",expected_dir("melband","Mel-Band-RoFormer")));
   CHECK(check("stem_live_buffered_filter","worker_model",expected_dir("demucs","Demucs")));
 }
 auto* defaults=obs_get_source_defaults("yamnet_classifier_source");
 CHECK(defaults&&!std::strlen(obs_data_get_string(defaults,"yamnet_model")));
 obs_data_release(defaults);
 if(!installed&&!fresh){
     fs::remove(models/"yamnet");
     CHECK(check("yamnet_classifier_source","yamnet_model",models));
     // A broken absolute override must not mask the existing family folder.
     fs::create_directories(models/"yamnet");
     CHECK(config_open(&c,path,CONFIG_OPEN_EXISTING)==CONFIG_SUCCESS);
     config_set_string(c,"ModelDirectories","yamnet",(base/"missing override").string().c_str());
     CHECK(config_save(c)==CONFIG_SUCCESS);config_close(c);
     CHECK(check("yamnet_classifier_source","yamnet_model",models/"yamnet"));
 }
 // Clear and missing directory must not force an invalid picker or load model.
 if(!installed&&!fresh){CHECK(config_open(&c,path,CONFIG_OPEN_EXISTING)==CONFIG_SUCCESS);
 config_set_string(c,"General","models",(base/"missing").string().c_str());
 CHECK(config_save(c)==CONFIG_SUCCESS);config_close(c);
 CHECK(check("yamnet_classifier_source","yamnet_model",fs::path(path).parent_path()/"models"));}
 if(fresh){CHECK(fs::is_directory(models));CHECK(fs::is_empty(models));CHECK(!fs::exists(path));}
 bfree(path);obs_shutdown();
 puts("SOUND_MODEL_PICKER_DEFAULTS_PASS=1");
}
