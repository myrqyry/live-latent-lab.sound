// Small helpers for safe OBS settings access
#pragma once

#include <string>
#include <obs-module.h>

static inline std::string safe_obs_data_get_string(obs_data_t *data, const char *key)
{
    if (!data || !key) return std::string();
    const char *v = obs_data_get_string(data, key);
    return v ? std::string(v) : std::string();
}
