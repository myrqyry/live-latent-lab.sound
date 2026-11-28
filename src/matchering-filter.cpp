#include <obs-module.h>
#include <obs.h>
#include <string>
#include <mutex>
#include "engines/matchering_engine.hpp"
#include "framework/obs_helpers.h"

struct matchering_data {
    obs_source_t *context;
    stts::MatcheringEngine engine; // exclusively owned by audio after creation
    stts::MatcheringControl control;
    std::mutex mutex; // serializes control writers, never taken by audio
    std::string reference_path;
    stts::MatcheringControl::Snapshot settings;
    uint32_t applied_generation = UINT32_MAX;
    uint32_t sample_rate = 48000;
};

static const char *matchering_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "lll.sound Matchering (Audio Match)";
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
        stts::MatcheringEngine loader;
        if (loader.initialize(filter->reference_path))
            filter->settings.reference = loader.reference_profile();
        else
            filter->settings.reference = {};
        ++filter->settings.generation;
    }

    filter->settings.parameters.smoothing = (float)obs_data_get_double(settings, "smoothing");
    filter->settings.parameters.eq_amount = (float)obs_data_get_double(settings, "eq_amount");
    filter->settings.parameters.mix = (float)obs_data_get_double(settings, "mix");

    filter->control.publish(filter->settings);
}

static void *matchering_create(obs_data_t *settings, obs_source_t *context)
{
    matchering_data *filter = new matchering_data();
    filter->context = context;
    if (auto* audio = obs_get_audio())
        filter->sample_rate = audio_output_get_sample_rate(audio);

    // Set defaults if not present
    matchering_update(filter, settings);
    return filter;
}

static struct obs_audio_data *matchering_filter_audio(void *data, struct obs_audio_data *audio)
{
    matchering_data *filter = (matchering_data *)data;

    if (!audio || !audio->data[0] || !audio->frames) return audio;

    stts::MatcheringControl::Snapshot snapshot;
    if (filter->control.try_read(snapshot)) {
        if (snapshot.generation != filter->applied_generation) {
            filter->engine.apply_reference(snapshot.reference);
            filter->applied_generation = snapshot.generation;
        }
        filter->engine.set_parameters(snapshot.parameters);
    }
    if (!filter->engine.is_ready()) return audio;

    auto* left = reinterpret_cast<float*>(audio->data[0]);
    auto* right = reinterpret_cast<float*>(audio->data[1]);
    filter->engine.update_analysis(left,right,audio->frames,filter->sample_rate);
    filter->engine.process_channel(left,audio->frames,0);
    if (right) filter->engine.process_channel(right,audio->frames,1);

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
