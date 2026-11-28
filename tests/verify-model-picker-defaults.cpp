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
 const bool expanded=argc==5&&std::string(argv[4])=="--expanded";
 const bool installed=argc==6&&std::string(argv[4])=="--read-only";
 fs::path base=argv[3],models=installed?fs::path(argv[5]):base/"model library",runtime=base/"runtime library";
 if(!installed){fs::create_directories(models);fs::create_directories(runtime);}
 CHECK(obs_startup("en-US",base.string().c_str(),nullptr));
 obs_module_t*m=nullptr;CHECK(obs_open_module(&m,argv[1],argv[2])==MODULE_SUCCESS&&obs_init_module(m));
 char* path=obs_module_get_config_path(m,"preferences.ini");CHECK(path);
 if(!installed)fs::create_directories(fs::path(path).parent_path());
 config_t* c=nullptr;
 if(!installed){CHECK(config_open(&c,path,CONFIG_OPEN_ALWAYS)==CONFIG_SUCCESS);
 config_set_string(c,"General","models",models.string().c_str());
 config_set_string(c,"General","runtimes",runtime.string().c_str());
 CHECK(config_save(c)==CONFIG_SUCCESS);config_close(c);}
 auto check=[&](const char* id,const char* key,const fs::path& expected){
     auto* p=obs_get_source_properties(id);
     if(!p)return false;
     auto* field=obs_properties_get(p,key);
     auto* value=field?obs_property_path_default_path(field):nullptr;
     bool ok=value&&expected.string()==value;
     printf("PICKER id=%s key=%s path=%s pass=%d\n",id,key,value?value:"(null)",ok);
     obs_properties_destroy(p);return ok;
 };
 CHECK(check("yamnet_classifier_source","yamnet_model",models));
 if(!installed)CHECK(check("yamnet_classifier_source","mediapipe_library",runtime));
 if(expanded){
   CHECK(check("sound_stt_filter","model_path",models));
   CHECK(check("sound_stt_filter","zipformer_model_dir",models));
   CHECK(check("sound_stt_filter","nemotron_model_path",models));
   CHECK(check("sound_stt_filter","parakeet_model_path",models));
   CHECK(check("sound_ddsp_filter","models",models));
   CHECK(check("stts_rvc_filter","model",models));
   CHECK(check("stts_rvc_filter","voice_checkpoint",models));
   CHECK(check("stts_rvc_filter","retrieval_index",models));
   CHECK(check("sound_audio_diffusion_source","models_dir",models));
   CHECK(check("sound_audio_diffusion_source","lora_a",models));
   CHECK(check("sound_audio_diffusion_source","lora_b",models));
 }
 auto* defaults=obs_get_source_defaults("yamnet_classifier_source");
 CHECK(defaults&&!std::strlen(obs_data_get_string(defaults,"yamnet_model")));
 obs_data_release(defaults);
 // Clear and missing directory must not force an invalid picker or load model.
 if(!installed){CHECK(config_open(&c,path,CONFIG_OPEN_EXISTING)==CONFIG_SUCCESS);
 config_set_string(c,"General","models",(base/"missing").string().c_str());
 CHECK(config_save(c)==CONFIG_SUCCESS);config_close(c);
 CHECK(check("yamnet_classifier_source","yamnet_model",{}));}
 bfree(path);obs_shutdown();
 puts("SOUND_MODEL_PICKER_DEFAULTS_PASS=1");
}
