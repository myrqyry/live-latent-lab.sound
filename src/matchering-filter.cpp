#include <obs-module.h>
#include <obs.h>
#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include "engines/matchering_engine.hpp"
#include "framework/obs_helpers.h"

struct matchering_data {
    obs_source_t *context;
    std::unique_ptr<stts::MatcheringEngine> engine;
    std::mutex mutex;
    std::string reference_path;
    stts::MatcheringEngine::Parameters params;
};

static const char *matchering_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "LLL.sound Matchering (Audio Match)";
}

static void matchering_destroy(void *data)
{
    matchering_data *filter = (matchering_data *)data;
    delete filter;
}

static void matchering_update(void *data, obs_data_t *settings)
{
    matchering_data *filter = (matchering_data *)data;
    std::lock_guard<std::mutex> lock(filter->mutex);

    const char* path_ptr = obs_data_get_string(settings, "reference_path");
    if (path_ptr && std::string(path_ptr) != filter->reference_path) {
        filter->reference_path = path_ptr;
        filter->engine->initialize(filter->reference_path);
    }

    filter->params.smoothing = (float)obs_data_get_double(settings, "smoothing");
    filter->params.eq_amount = (float)obs_data_get_double(settings, "eq_amount");
    filter->params.mix = (float)obs_data_get_double(settings, "mix");

    filter->engine->set_parameters(filter->params);
}

static void *matchering_create(obs_data_t *settings, obs_source_t *context)
{
    matchering_data *filter = new matchering_data();
    filter->context = context;
    filter->engine = std::make_unique<stts::MatcheringEngine>();

    // Set defaults if not present
    matchering_update(filter, settings);
    return filter;
}

static struct obs_audio_data *matchering_filter_audio(void *data, struct obs_audio_data *audio)
{
    matchering_data *filter = (matchering_data *)data;

    if (!filter->engine->is_ready()) {
        return audio;
    }

    std::lock_guard<std::mutex> lock(filter->mutex);

    uint32_t channels = audio_output_get_channels(obs_get_audio());
    // Fallback if we can't get global channels, guess based on data pointers?
    // audio->frames is count.
    // We assume max 2 channels for this filter or just process first 2.
    // OBS usually processes in planar or interleaved. obs_audio_data is planar (array of pointers).

    size_t frames = audio->frames;

    // 1. Analyze Mix (Mono Downmix)
    std::vector<float> mix_input(frames);
    float *l = (float *)audio->data[0];

    if (channels >= 2 && audio->data[1]) {
        float *r = (float *)audio->data[1];
        for (size_t i = 0; i < frames; i++) {
            mix_input[i] = (l[i] + r[i]) * 0.5f;
        }
    } else {
        // Mono
        memcpy(mix_input.data(), l, frames * sizeof(float));
    }

    // Update Analysis State
    uint32_t sample_rate = audio_output_get_sample_rate(obs_get_audio());
    filter->engine->update_analysis(mix_input, sample_rate);

    // 2. Process Channels
    // Channel 0 (Left / Mono)
    {
        std::vector<float> ch_data(l, l + frames);
        filter->engine->process_channel(ch_data, 0);
        memcpy(l, ch_data.data(), frames * sizeof(float));
    }

    // Channel 1 (Right)
    if (channels >= 2 && audio->data[1]) {
        float *r = (float *)audio->data[1];
        std::vector<float> ch_data(r, r + frames);
        filter->engine->process_channel(ch_data, 1);
        memcpy(r, ch_data.data(), frames * sizeof(float));
    }

    return audio;
}

static obs_properties_t *matchering_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "reference_path", "Reference Audio File (.wav)",
                            OBS_PATH_FILE, "*.wav", nullptr);

    obs_properties_add_float_slider(props, "eq_amount", "Match Amount", 0.0, 1.0, 0.01);
    obs_properties_add_float_slider(props, "smoothing", "Smoothing", 0.0, 1.0, 0.01);
    obs_properties_add_float_slider(props, "mix", "Dry/Wet Mix", 0.0, 1.0, 0.01);

    obs_properties_add_text(props, "info",
        obs_module_text("MatcheringInfo"), OBS_TEXT_INFO);

    return props;
}

static void matchering_defaults(obs_data_t *settings)
{
    obs_data_set_default_double(settings, "eq_amount", 1.0);
    obs_data_set_default_double(settings, "smoothing", 0.5);
    obs_data_set_default_double(settings, "mix", 1.0);
}

extern "C" struct obs_source_info matchering_filter = {
    .id = "matchering_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO,
    .get_name = matchering_get_name,
    .create = matchering_create,
    .destroy = matchering_destroy,
    .get_defaults = matchering_defaults,
    .get_properties = matchering_properties,
    .update = matchering_update,
    .filter_audio = matchering_filter_audio,
};
