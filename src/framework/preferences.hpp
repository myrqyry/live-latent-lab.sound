#pragma once
// Read model/runtime browsing preferences even in the Qt-free module.
// Only called from property/control paths, never the audio callback.
#include <obs-module.h>
#include <util/config-file.h>
#include <util/bmem.h>
#include <filesystem>
#include <string>

namespace sound_preferences {
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
    return dir.is_absolute()&&std::filesystem::is_directory(dir,ec)?result:std::string{};
}
}
