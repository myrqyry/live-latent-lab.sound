#pragma once
// Read model/runtime browsing preferences even in the Qt-free module.
// Only called from property/control paths, never the audio callback.
#include <obs-module.h>
#include <util/config-file.h>
#include <util/bmem.h>
#include <filesystem>
#include <string>

namespace sound_preferences {
#ifdef SOUND_WITH_SETTINGS_PAGE
// Older Qt-written INI values may use QSettings-specific quoting/escapes.
std::string legacy_directory(bool runtime);
#endif
inline std::string directory(bool runtime=false){
    char* path=obs_module_config_path("preferences.ini");
    if(!path)return {};
    config_t* config=nullptr;
    const auto status=config_open(&config,path,CONFIG_OPEN_EXISTING);
    bfree(path);
    if(status!=CONFIG_SUCCESS)return {};
    const char* value=config_get_string(config,"General",runtime?"runtimes":"models");
    std::string result=value?value:"";
    config_close(config);
    if(result.empty())return {};
    std::error_code ec;
    auto dir=std::filesystem::u8path(result);
    if(dir.is_absolute()&&std::filesystem::is_directory(dir,ec))return result;
#ifdef SOUND_WITH_SETTINGS_PAGE
    return legacy_directory(runtime);
#else
    return {};
#endif
}
// Browse the matching family directory, even on a fresh installation.
inline std::string model_directory(const char* family,const char* relative){
    namespace fs=std::filesystem;
    auto valid=[](const fs::path& p){std::error_code ec;return p.is_absolute()&&fs::is_directory(p,ec);};
    char* path=obs_module_config_path("preferences.ini");
    config_t* config=nullptr;
    std::string override_dir;
    if(path&&config_open(&config,path,CONFIG_OPEN_EXISTING)==CONFIG_SUCCESS){
        const char* value=config_get_string(config,"ModelDirectories",family);
        if(value)override_dir=value;
        config_close(config);
    }
    bfree(path);
    if(!override_dir.empty()&&valid(fs::u8path(override_dir)))return override_dir;
    auto root=directory(false);
    if(!root.empty()){
        auto specific=fs::u8path(root)/relative;
        if(valid(specific))return specific.u8string();
    }
    // A fresh user has neither preferences.ini nor model directories.
    // OBS's user-owned plugin config directory is writable even when the
    // installed plugin data directory is read-only (e.g. system packages).
    // Lazily create only the family being requested, with no model download.
    auto local_relative=std::string("models/")+relative;
    char* local=obs_module_config_path(local_relative.c_str());
    std::string local_dir=local?local:"";
    bfree(local);
    if(!local_dir.empty()){
        auto dir=fs::u8path(local_dir);
        if(dir.is_absolute()){
            std::error_code ec;
            fs::create_directories(dir,ec);
            if(valid(dir))return local_dir;
        }
    }
    // Permission errors or an obstructing non-directory: never hand OBS
    // a nonexistent path. Use the configured root or the system chooser.
    return root;
}
}
