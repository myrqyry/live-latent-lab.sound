// Uses real libobs audio initialization and the real filter callbacks,
// compiled from plugin sources. No fake inference engines, no model files.
// Verifies dry path for unconfigured AI filters and channel independence
// for the microphone DSP filter.
#include <obs.h>
#include <obs-module.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

extern struct obs_source_info stts_rvc_filter;
extern "C" struct obs_source_info deepfilter_filter;
extern "C" struct obs_source_info mic_filter;

constexpr size_t N = 512;
using Samples = std::array<float, N>;

static bool process(struct obs_source_info &info, obs_data_t *settings,
                    Samples &left, Samples &right)
{
    if (info.get_defaults) info.get_defaults(settings);
    void *state = info.create(settings, nullptr);
    if (!state) return false;
    struct obs_audio_data audio{};
    audio.frames = N;
    audio.data[0] = reinterpret_cast<uint8_t*>(left.data());
    audio.data[1] = reinterpret_cast<uint8_t*>(right.data());
    auto *out = info.filter_audio(state, &audio);
    bool ok = out == &audio && audio.data[0] == reinterpret_cast<uint8_t*>(left.data()) &&
              audio.data[1] == reinterpret_cast<uint8_t*>(right.data());
    info.destroy(state);
    return ok;
}

int main()
{
    if (!obs_startup("en-US", nullptr, nullptr)) return 2;
    struct obs_audio_info ai{};
    ai.samples_per_sec = 48000;
    ai.speakers = SPEAKERS_STEREO;
    if (!obs_reset_audio(&ai)) { obs_shutdown(); return 2; }

    Samples reference_left{}, reference_right{};
    for (size_t i = 0; i < N; ++i) {
        reference_left[i] = .45f * std::sin(float(i) * .83f);
        reference_right[i] = -.22f * std::cos(float(i) * .17f);
    }

    bool df_dry = false, stts_dry = false, mic_stereo = false;
    {
        auto *settings = obs_data_create();
        Samples l = reference_left, r = reference_right;
        df_dry = process(deepfilter_filter, settings, l, r) &&
                 l == reference_left && r == reference_right;
        obs_data_release(settings);
    }
    {
        auto *settings = obs_data_create();
        Samples l = reference_left, r = reference_right;
        stts_dry = process(stts_rvc_filter, settings, l, r) &&
                   l == reference_left && r == reference_right;
        obs_data_release(settings);
    }
    {
        auto *settings = obs_data_create();
        if (mic_filter.get_defaults) mic_filter.get_defaults(settings);
        obs_data_set_int(settings, "mic_type", 3); // Ribbon
        Samples l = reference_left, r{};
        Samples expected_right = r;
        mic_stereo = process(mic_filter, settings, l, r) &&
                     r == expected_right && l != reference_left;
        obs_data_release(settings);
    }
    obs_shutdown();
    std::printf("SOUND_AUDIO_CALLBACK df_missing_model_dry=%d stts_missing_model_dry=%d independent_stereo_dsp=%d\n",
                df_dry, stts_dry, mic_stereo);
    bool pass = df_dry && stts_dry && mic_stereo;
    std::printf("SOUND_AUDIO_CALLBACK_PASS=%d\n", pass);
    return pass ? 0 : 1;
}
