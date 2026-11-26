#pragma once
#include <string>
namespace sound_preferences {
#ifdef SOUND_WITH_SETTINGS_PAGE
std::string directory(bool runtime=false);
#else
inline std::string directory(bool=false){return {};}
#endif
}
