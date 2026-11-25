#pragma once
#include <string>

namespace stts {

// Model/session ownership stays separate from settings consumed for each
// utterance. The OBS worker is the sole owner of its active copy.
struct WhisperDecode {
    bool beam=false,suppress_blank=true,suppress_nst=false;
    int best_of=5,beam_size=5;
    float temperature=0,temperature_increment=.2f,entropy=2.4f,logprob=-1.f,no_speech=.6f;
};
struct ZipformerDecode {
    bool beam=false;
    int paths=4;
    float blank_penalty=0;
    bool operator!=(const ZipformerDecode& b)const{
        return beam!=b.beam||paths!=b.paths||blank_penalty!=b.blank_penalty;
    }
};
struct SttConfig {
    WhisperDecode whisper_decode;
    ZipformerDecode zipformer_decode;
    std::string backend="whisper",model,parakeet_model,parakeet_library,target,language="en",prompt;
    std::string whisper_device="auto";
    std::string nemotron_model,nemotron_library,nemotron_language="en-US",nemotron_device="auto";
    std::string parakeet_device="auto";
    std::string zipformer_model;
    int zipformer_threads=2;
    int nemotron_threads=4,nemotron_right=3;
    int threads=4,threshold=-45,silence=600,max_phrase=8000,hold=5000,caption_lines=1;
    bool translate=false,while_muted=false;
};

inline bool requires_model_reload(const SttConfig& a,const SttConfig& b) {
    if(a.backend!=b.backend)return true;
    if(a.backend=="whisper")
        return a.model!=b.model || a.whisper_device!=b.whisper_device ||
            a.language!=b.language || a.threads!=b.threads || a.translate!=b.translate;
    if(a.backend=="parakeet")
        return a.parakeet_model!=b.parakeet_model || a.parakeet_library!=b.parakeet_library ||
            a.parakeet_device!=b.parakeet_device;
    if(a.backend=="zipformer")return a.zipformer_model!=b.zipformer_model||a.zipformer_threads!=b.zipformer_threads||a.zipformer_decode!=b.zipformer_decode;
    if(a.backend=="nemotron")
        return a.nemotron_model!=b.nemotron_model || a.nemotron_library!=b.nemotron_library ||
            a.nemotron_language!=b.nemotron_language || a.nemotron_device!=b.nemotron_device ||
            a.nemotron_threads!=b.nemotron_threads || a.nemotron_right!=b.nemotron_right;
    return false;
}

} // namespace stts
