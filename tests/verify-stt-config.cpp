#include "framework/stt_config.hpp"
#include <cassert>
#include <iostream>

int main() {
    stts::SttConfig original;
    original.model = "whisper.bin";
    original.parakeet_model = "parakeet.litertlm";
    original.nemotron_model = "nemotron.gguf";

    auto live = original;
    live.threshold = -36;
    live.silence = 900;
    live.max_phrase = 6000;
    live.hold = 12000;
    live.target = "different-caption-uuid";
    live.prompt = "streamer names";
    live.while_muted = true;
    assert(!stts::requires_model_reload(original, live));

    auto cold = original;
    cold.backend = "parakeet";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.model = "another-whisper.bin";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.language = "es";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.translate = true;
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.threads = 8;
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.parakeet_model = "another-parakeet.litertlm";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.parakeet_library = "libmediapipe.so";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.nemotron_model = "another-nemotron.gguf";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.nemotron_library = "libnemotron.so";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.nemotron_language = "es-ES";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.nemotron_threads = 6;
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.nemotron_device = "cpu";
    assert(stts::requires_model_reload(original, cold));
    cold = original; cold.nemotron_right = 6;
    assert(stts::requires_model_reload(original, cold));
    assert(!stts::requires_model_reload(original, original));
    std::cout << "SOUND_STT_CONFIG_POLICY_PASS=1\n";
}
