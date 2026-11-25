#include <obs-module.h>
#include <plugin-support.h>

#if defined(SOUND_WITH_DIFFUSION)
extern void register_sound_diffusion();
#ifdef SOUND_WITH_DIFFUSION_DOCK
void register_diffusion_dock();
void unregister_diffusion_dock();
#endif
#endif

#if defined(SOUND_WITH_DDSP)
extern void register_sound_ddsp();
#endif

// Compiled-source and registration gates match exactly. No source is even
// linked into the default module, so unchecked effects cannot appear in OBS.
#if defined(SOUND_WITH_TTS)
extern void register_sound_tts();
#endif
#if defined(SOUND_WITH_RVC)
extern struct obs_source_info stts_rvc_filter;
#endif
#if defined(SOUND_WITH_MEANVC2_BRIDGE)
extern struct obs_source_info meanvc2_bridge_source;
#endif
#if defined(SOUND_WITH_DEMUCS_FILTER)
extern struct obs_source_info demucs_filter;
#endif
#if defined(SOUND_WITH_MIC)
extern struct obs_source_info mic_filter;
#endif
#if defined(SOUND_WITH_STEM_LIVE)
extern struct obs_source_info stem_live_filter;
#endif
#if defined(SOUND_WITH_DEEPFILTER)
extern struct obs_source_info deepfilter_filter;
#endif
#if defined(SOUND_WITH_MATCHERING)
extern struct obs_source_info matchering_filter;
#endif
#if defined(SOUND_WITH_DEMUCS_SOURCE)
extern struct obs_source_info demucs_source;
extern struct obs_source_info demucs_stem_source;
#endif
#if defined(SOUND_WITH_CHATTERBOX)
extern struct obs_source_info chatterbox_source;
#endif
#if defined(SOUND_WITH_KOKORO)
extern struct obs_source_info kokoro_source;
#endif
#if defined(SOUND_WITH_MUSICGEN)
extern struct obs_source_info musicgen_source;
#endif
#if defined(SOUND_WITH_STT)
extern "C" struct obs_source_info sound_stt_filter;
#endif
#if defined(SOUND_WITH_WHISPER)
extern struct obs_source_info whisper_source;
#endif
#if defined(SOUND_WITH_YAMNET)
extern struct obs_source_info yamnet_classifier_source;
#endif
#if defined(SOUND_WITH_MEDIAPIPE)
extern struct obs_source_info mediapipe_filter;
#endif
#if defined(SOUND_WITH_TOOLS_MENU)
#include "optimizer_ui.h"
void log_qt_version();
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
    // Mic-only candidates have unique configure-time markers; the default
    // quarantined shell retains its original log marker. Verify actual OBS
    // startup logs before claiming that a newly built module is installed.
#if defined(SOUND_STEM_BUILD_MARKER)
    obs_log(LOG_INFO, "%s module loaded: opt-in buffered stem filter build", SOUND_STEM_BUILD_MARKER);
#elif defined(SOUND_YAMNET_BUILD_MARKER)
    obs_log(LOG_INFO, "%s module loaded: experimental async native YAMNet only",
            SOUND_YAMNET_BUILD_MARKER);
#elif defined(SOUND_MIC_ONLY_MARKER)
    obs_log(LOG_INFO, "%s module loaded: verified mic DSP only", SOUND_MIC_ONLY_MARKER);
#else
    obs_log(LOG_INFO, "SOUND_QUARANTINE_GATE_20261013 module loaded");
#endif
#if defined(SOUND_WITH_DDSP)
    register_sound_ddsp();
#endif
#if defined(SOUND_WITH_MEANVC2_BRIDGE)
    obs_register_source(&meanvc2_bridge_source);
#endif
#if defined(SOUND_WITH_TOOLS_MENU)
    log_qt_version();
    init_optimizer_tools_menu();
#endif
#if defined(SOUND_WITH_RVC)
    obs_register_source(&stts_rvc_filter);
#endif
#if defined(SOUND_WITH_DEMUCS_FILTER)
    obs_register_source(&demucs_filter);
#endif
#if defined(SOUND_WITH_MIC)
    obs_register_source(&mic_filter);
#endif
#if defined(SOUND_WITH_STEM_LIVE)
    obs_register_source(&stem_live_filter);
#endif
#if defined(SOUND_WITH_DEEPFILTER)
    obs_register_source(&deepfilter_filter);
#endif
#if defined(SOUND_WITH_MATCHERING)
    obs_register_source(&matchering_filter);
#endif
#if defined(SOUND_WITH_DEMUCS_SOURCE)
    // Child stem source must be registered before the parent can create it.
    obs_register_source(&demucs_stem_source);
    obs_register_source(&demucs_source);
#endif
#if defined(SOUND_WITH_CHATTERBOX)
    obs_register_source(&chatterbox_source);
#endif
#if defined(SOUND_WITH_KOKORO)
    obs_register_source(&kokoro_source);
#endif
#if defined(SOUND_WITH_MUSICGEN)
    obs_register_source(&musicgen_source);
#endif
#if defined(SOUND_WITH_WHISPER)
    obs_register_source(&whisper_source);
#endif
#if defined(SOUND_WITH_STT)
    obs_register_source(&sound_stt_filter);
    obs_log(LOG_INFO,"SOUND_STT_FILTER_20261118_S813");
    obs_log(LOG_INFO,"SOUND_STT_PARAKEET_20261119_P614");
    obs_log(LOG_INFO,"SOUND_STT_NEMOTRON_20261119_N560");
#endif
#if defined(SOUND_WITH_YAMNET)
    obs_register_source(&yamnet_classifier_source);
#endif
#if defined(SOUND_WITH_MEDIAPIPE)
    obs_register_source(&mediapipe_filter);
#endif
#if defined(SOUND_WITH_TTS)
    register_sound_tts();
#endif
#if defined(SOUND_WITH_DIFFUSION)
    register_sound_diffusion();
#endif
    return true;
}

#ifdef SOUND_WITH_DIFFUSION_DOCK
void obs_module_post_load(void)
{
    register_diffusion_dock();
}
#endif

void obs_module_unload(void)
{
#ifdef SOUND_WITH_DIFFUSION_DOCK
    unregister_diffusion_dock();
#endif
    obs_log(LOG_INFO, "Sound quarantine module unloaded");
}
