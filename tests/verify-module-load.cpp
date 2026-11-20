// Real libobs module registration smoke test.
// Expected set is explicit; all non-selected historical effect IDs must be absent.
#include <obs.h>
#include <obs-module.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Effect { const char *id; const char *gate; };
static const Effect effects[] = {
    {"stts_rvc_filter", "rvc"},
    {"meanvc2_bridge_source", "meanvc2_bridge"},
    {"demucs_audio_filter", "demucs_filter"},
    {"mic_modeler_filter", "mic"},
    {"stem_live_buffered_filter", "stem_live"},
    {"deepfilter_noise_suppression", "deepfilter"},
    {"matchering_filter", "matchering"},
    {"demucs_stem_output", "demucs_source"},
    {"demucs_stem_source", "demucs_source"},
    {"chatterbox_source", "chatterbox"},
    {"kokoro_tts_source", "kokoro"},
    {"musicgen_source", "musicgen"},
    {"whisper_text_source", "whisper"},
    {"sound_stt_filter", "stt"},
    {"mediapipe_filter", "mediapipe"},
    {"yamnet_classifier_source", "yamnet"},
    {"sound_kokoro_tts_source", "tts"},
    {"sound_piper_tts_source", "tts"},
    {"sound_chatterbox_tts_source", "tts"},
    {"sound_tiktok_tts_source", "tts"},
    {"sound_tts_source", "tts"},
};

static bool contains(const std::string &list, const char *candidate)
{
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        if (end == std::string::npos) end = list.size();
        if (list.substr(start, end - start) == candidate) return true;
        if (end == list.size()) break;
        start = end + 1;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: check MODULE.so DATA_DIR EXPECTED_EFFECTS_SEMICOLON_LIST\n");
        return 2;
    }
    const std::string expected = argv[3];
    if (!obs_startup("en-US", nullptr, nullptr)) return 2;

    obs_module_t *module = nullptr;
    int open = obs_open_module(&module, argv[1], argv[2]);
    if (open != MODULE_SUCCESS) {
        std::fprintf(stderr, "SOUND_MODULE_OPEN_FAILED=%d\n", open);
        obs_shutdown();
        return 3;
    }
    bool initialized = obs_init_module(module);
    bool matched = initialized;
    int registered = 0;
    for (const auto &item : effects) {
        obs_properties_t *props = initialized ? obs_get_source_properties(item.id) : nullptr;
        const bool found = props != nullptr;
        const bool wanted = contains(expected, item.gate);
        if (found) ++registered;
        if (found != wanted) {
            std::fprintf(stderr, "REGISTRY_MISMATCH id=%s expected=%d actual=%d\n",
                         item.id, wanted, found);
            matched = false;
        }
        if (props) obs_properties_destroy(props);
    }
    std::printf("SOUND_REGISTRY_MATCH=%d EXPECTED=[%s] ACTUAL_COUNT=%d\n",
                matched, expected.c_str(), registered);
    obs_shutdown();
    return matched ? 0 : 1;
}
