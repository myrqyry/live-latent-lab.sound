#pragma once
#include <string>

namespace stts {

// Model/session ownership stays separate from settings consumed for each
// utterance. The OBS worker is the sole owner of its active copy.
struct SttConfig {
    std::string backend="whisper",model,parakeet_model,parakeet_library,target,language="en",prompt;
    std::string nemotron_model,nemotron_library,nemotron_language="en-US";
    int nemotron_threads=4,nemotron_right=3;
    int threads=4,threshold=-45,silence=600,max_phrase=8000,hold=5000;
    bool translate=false,while_muted=false;
};

inline bool requires_model_reload(const SttConfig& a,const SttConfig& b) {
    return a.backend!=b.backend || a.model!=b.model ||
        a.parakeet_model!=b.parakeet_model || a.parakeet_library!=b.parakeet_library ||
        a.nemotron_model!=b.nemotron_model || a.nemotron_library!=b.nemotron_library ||
        a.nemotron_language!=b.nemotron_language ||
        a.nemotron_threads!=b.nemotron_threads || a.nemotron_right!=b.nemotron_right ||
        a.language!=b.language || a.threads!=b.threads || a.translate!=b.translate;
}

} // namespace stts
