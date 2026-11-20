#include <obs-module.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <fstream>
#include <cstring>

#include <util/platform.h>
#include <util/threading.h>
#include "engines/chatterbox_engine.hpp"
#include "framework/ring-buffer.hpp"
#include "framework/audio_utils.hpp"
#include "framework/obs_helpers.h"

/* ------------------------------------------------------------------------- */
/* WAV Loading Helper                                                        */
/* ------------------------------------------------------------------------- */

struct WavData {
    std::vector<float> samples;
    uint32_t sample_rate;
};

static WavData load_wav_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {{}, 0};

    // Basic WAV header parsing
    char chunkId[4];
    file.read(chunkId, 4);
    if (std::strncmp(chunkId, "RIFF", 4) != 0) return {{}, 0};

    // Scan chunks to find fmt and data
    file.seekg(12);

    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    bool fmtFound = false;

    while (file.good()) {
        char chunkName[4];
        file.read(chunkName, 4);
        if (file.gcount() < 4) break;

        uint32_t chunkSize;
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (file.gcount() < 4) break;

        // Header chunk
        if (std::strncmp(chunkName, "fmt ", 4) == 0) {
            if (chunkSize < 16) return {{}, 0}; // Invalid fmt chunk

            // Format tag (2), Channels (2), SamplesPerSec (4), AvgBytesPerSec (4), BlockAlign (2), BitsPerSample (2)
            uint16_t fmtTag;
            file.read(reinterpret_cast<char*>(&fmtTag), 2);
            file.read(reinterpret_cast<char*>(&channels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            file.seekg(6, std::ios::cur); // Skip AvgBytesPerSec, BlockAlign
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);

            // Skip remaining bytes in fmt chunk if any
            if (chunkSize > 16) {
                file.seekg(chunkSize - 16, std::ios::cur);
            }
            fmtFound = true;
        }
        else if (std::strncmp(chunkName, "data", 4) == 0) {
            if (!fmtFound || channels == 0 || sampleRate == 0) return {{}, 0};

            // Read data
            std::vector<uint8_t> buffer(chunkSize);
            file.read(reinterpret_cast<char*>(buffer.data()), chunkSize);
            if (file.gcount() != chunkSize) return {{}, 0};

            // Convert to float
            std::vector<float> output;
            if (bitsPerSample == 16) {
                int16_t* samples = reinterpret_cast<int16_t*>(buffer.data());
                size_t numSamples = chunkSize / 2;
                output.reserve(numSamples / channels); // We'll take first channel
                for (size_t i = 0; i < numSamples; i += channels) {
                    output.push_back(static_cast<float>(samples[i]) / 32768.0f);
                }
            } else if (bitsPerSample == 32) { // Float
                float* samples = reinterpret_cast<float*>(buffer.data());
                size_t numSamples = chunkSize / 4;
                output.reserve(numSamples / channels);
                for (size_t i = 0; i < numSamples; i += channels) {
                    output.push_back(samples[i]);
                }
            }

            return {output, sampleRate};
        } else {
            file.seekg(chunkSize, std::ios::cur);
        }
    }
    return {{}, 0};
}

/* ------------------------------------------------------------------------- */
/* Data Structure                                                            */
/* ------------------------------------------------------------------------- */

struct chatterbox_source_data {
    obs_source_t *context;

    // Engine
    std::unique_ptr<stts::ChatterboxEngine> engine;

    // Model Paths
    std::string speech_encoder_path;
    std::string embed_tokens_path;
    std::string language_model_path;
    std::string decoder_path;
    std::string tokenizer_vocab_path;
    std::string tokenizer_merges_path;

    // Config
    std::string text_to_speak;
    std::string reference_audio_path;

    // Audio Buffering (Ring Buffer pattern - matches Kokoro)
    std::unique_ptr<stts::RingBuffer> audio_ring;
    uint32_t obs_sample_rate;     // OBS output sample rate
    uint32_t engine_sample_rate;  // Engine output rate (typically 24kHz)

    // Worker Thread (Event-driven pattern - matches Kokoro)
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    os_event_t *work_signal;

    // Safety
    std::mutex engine_mutex;
    std::mutex text_mutex;
};

/* ------------------------------------------------------------------------- */
/* Worker Thread (Event-driven, not polling)                                 */
/* ------------------------------------------------------------------------- */

static void chatterbox_process_thread(chatterbox_source_data *data) {
    while (!data->stop_thread) {
        // Wait for signal instead of polling
        os_event_wait(data->work_signal);
        if (data->stop_thread) break;

        // Get text to speak
        std::string text;
        std::string ref_path;
        {
            std::lock_guard<std::mutex> lock(data->text_mutex);
            text = data->text_to_speak;
            data->text_to_speak.clear();
            ref_path = data->reference_audio_path;
        }

        if (text.empty()) continue;

        // Load reference audio
        WavData ref_wav = {{}, 0};
        if (!ref_path.empty()) {
            ref_wav = load_wav_file(ref_path);
        }

        // Fallback if load failed or path empty
        if (ref_wav.samples.empty()) {
            ref_wav.samples.resize(24000 * 3, 0.0f); // 3 seconds of silence
            ref_wav.sample_rate = 24000;
        }

        std::vector<float> generated;
        {
            std::lock_guard<std::mutex> e_lock(data->engine_mutex);
            if (data->engine && data->engine->is_ready()) {
                data->engine->set_text(text);
                // Pass the sample rate of the reference audio file
                generated = data->engine->process(ref_wav.samples, ref_wav.sample_rate);
            }
        }

        if (generated.empty()) continue;

        // Resample if engine output rate differs from OBS output rate
        std::vector<float> output = generated;
        if (data->engine_sample_rate != data->obs_sample_rate) {
            output = stts::Resampler::resample(generated, data->engine_sample_rate, data->obs_sample_rate);
        }

        // Write to ring buffer (thread-safe)
        if (!output.empty()) {
            data->audio_ring->write(output.data(), output.size());
        }
    }
}

/* ------------------------------------------------------------------------- */
/* OBS UI Properties                                                         */
/* ------------------------------------------------------------------------- */

static obs_properties_t *chatterbox_properties(void *data_ptr) {
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();

    obs_properties_add_path(props, "speech_encoder", "Speech Encoder (.onnx)", OBS_PATH_FILE, "*.onnx", nullptr);
    obs_properties_add_path(props, "embed_tokens", "Embed Tokens (.onnx)", OBS_PATH_FILE, "*.onnx", nullptr);
    obs_properties_add_path(props, "language_model", "Language Model (.onnx)", OBS_PATH_FILE, "*.onnx", nullptr);
    obs_properties_add_path(props, "decoder", "Decoder (.onnx)", OBS_PATH_FILE, "*.onnx", nullptr);

    obs_properties_add_path(props, "tokenizer_vocab", "Tokenizer Vocab (.json)", OBS_PATH_FILE, "*.json", nullptr);
    obs_properties_add_path(props, "tokenizer_merges", "Tokenizer Merges (.txt)", OBS_PATH_FILE, "*.txt", nullptr);

    obs_properties_add_path(props, "ref_audio", "Reference Voice (.wav)", OBS_PATH_FILE, "*.wav", nullptr);

    obs_properties_add_text(props, "text_input", obs_module_text("TextToSpeak"), OBS_TEXT_MULTILINE);

    obs_properties_add_button(props, "generate_btn", "Generate Speech", [](obs_properties_t *p_props, obs_property_t *p_property, void *data_ptr) -> bool {
        UNUSED_PARAMETER(p_props);
        UNUSED_PARAMETER(p_property);
        chatterbox_source_data *data = (chatterbox_source_data *)data_ptr;
        
        // Get current text from settings
        obs_data_t *settings = obs_source_get_settings(data->context);
        const char* text_ptr = obs_data_get_string(settings, "text_input");
        if (text_ptr && *text_ptr) {
            std::string text = text_ptr;
            std::lock_guard<std::mutex> lock(data->text_mutex);
            data->text_to_speak = text;
            os_event_signal(data->work_signal);
        }
        obs_data_release(settings);
        return false; // Return false to prevent settings from being updated by the button press
    });

    obs_properties_add_int(props, "num_threads", "Inference Threads", 1, 16, 1);

    obs_property_t *backend = obs_properties_add_list(props, "backend", "Compute Backend",
                                                      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(backend, "Auto (Recommended)", "auto");
    obs_property_list_add_string(backend, "NVIDIA CUDA", "cuda");
    obs_property_list_add_string(backend, "CPU", "cpu");

    return props;
}

static void chatterbox_update(void *data_ptr, obs_data_t *settings) {
    chatterbox_source_data *data = (chatterbox_source_data *)data_ptr;

        std::string se = safe_obs_data_get_string(settings, "speech_encoder");
        std::string et = safe_obs_data_get_string(settings, "embed_tokens");
        std::string lm = safe_obs_data_get_string(settings, "language_model");
        std::string dc = safe_obs_data_get_string(settings, "decoder");
        std::string tv = safe_obs_data_get_string(settings, "tokenizer_vocab");
        std::string tm = safe_obs_data_get_string(settings, "tokenizer_merges");

    {
        std::lock_guard<std::mutex> lock(data->text_mutex);
        data->text_to_speak = safe_obs_data_get_string(settings, "text_input");
        data->reference_audio_path = safe_obs_data_get_string(settings, "ref_audio");
    }

    bool reload = (se != data->speech_encoder_path || et != data->embed_tokens_path ||
                   lm != data->language_model_path || dc != data->decoder_path);

    // Check if tokenizer changed, reload engine or just tokenizer?
    // The engine handles tokenizer loading separately but currently initialize() doesn't take tokenizer paths.
    // We should probably just reload if paths change.

    bool tokenizer_changed = (tv != data->tokenizer_vocab_path || tm != data->tokenizer_merges_path);

    if (reload || tokenizer_changed) {
        data->speech_encoder_path = se;
        data->embed_tokens_path = et;
        data->language_model_path = lm;
        data->decoder_path = dc;
        data->tokenizer_vocab_path = tv;
        data->tokenizer_merges_path = tm;

        std::lock_guard<std::mutex> e_lock(data->engine_mutex);
        if (!data->engine) {
            data->engine = std::make_unique<stts::ChatterboxEngine>();
        }

        std::string backend = safe_obs_data_get_string(settings, "backend");
        int threads = (int)obs_data_get_int(settings, "num_threads");

        std::stringstream ss;
        ss << se << "|" << et << "|" << lm << "|" << dc << "|" << threads << "|" << (backend.empty() ? "auto" : backend);

        if (reload) {
            data->engine->initialize(ss.str());
        }

        // Load tokenizer (it's safe to call multiple times)
        if (!tv.empty() && !tm.empty()) {
            data->engine->load_tokenizer_vocab(tv, tm);
        }
    }
}

static const char *chatterbox_get_name(void *) {
    return "LLL.sound Voice Clone (Chatterbox)";
}

static void *chatterbox_create(obs_data_t *settings, obs_source_t *context) {
    chatterbox_source_data *data = new chatterbox_source_data();
    data->context = context;
    
    // Get actual OBS output sample rate instead of hardcoding
    data->obs_sample_rate = audio_output_get_sample_rate(obs_get_audio());
    data->engine_sample_rate = 24000; // Chatterbox outputs at 24kHz

    // 60 seconds ring buffer at OBS sample rate
    data->audio_ring = std::make_unique<stts::RingBuffer>(data->obs_sample_rate * 60);

    // Initialize event for worker thread signaling
    os_event_init(&data->work_signal, OS_EVENT_TYPE_AUTO);

    chatterbox_update(data, settings);

    // Single worker thread (event-driven, not polling)
    data->worker_thread = std::thread(chatterbox_process_thread, data);

    return data;
}

static void chatterbox_destroy(void *data_ptr) {
    chatterbox_source_data *data = (chatterbox_source_data *)data_ptr;
    
    data->stop_thread = true;
    os_event_signal(data->work_signal); // Wake up thread so it can exit
    
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    
    os_event_destroy(data->work_signal);
    delete data;
}

/* ------------------------------------------------------------------------- */
/* Audio Output via video_tick (matches Kokoro pattern)                      */
/* ------------------------------------------------------------------------- */

static void chatterbox_tick(void *data_ptr, float seconds) {
    UNUSED_PARAMETER(seconds);
    chatterbox_source_data *data = (chatterbox_source_data *)data_ptr;

    size_t avail = data->audio_ring->available();
    if (avail > 0) {
        // Read and output in chunks
        constexpr size_t kChunkSize = 1024;
        float buffer[kChunkSize];

        size_t read = data->audio_ring->read(buffer, kChunkSize);

        if (read > 0) {
            obs_source_audio audio = {};
            audio.data[0] = (const uint8_t*)buffer;
            audio.frames = (uint32_t)read;
            audio.speakers = SPEAKERS_MONO;
            audio.samples_per_sec = data->obs_sample_rate;
            audio.format = AUDIO_FORMAT_FLOAT;
            audio.timestamp = os_gettime_ns();

            obs_source_output_audio(data->context, &audio);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Plugin Registration                                                       */
/* ------------------------------------------------------------------------- */

struct obs_source_info chatterbox_source = {
    .id = "chatterbox_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = chatterbox_get_name,
    .create = chatterbox_create,
    .destroy = chatterbox_destroy,
    .get_properties = chatterbox_properties,
    .update = chatterbox_update,
    .video_tick = chatterbox_tick,
};
