# Neural Timbre (DDSP)

An opt-in monophonic timbre-transfer audio filter in Sound. It evaluates
real Magenta DDSP-VST neural features and instrument control decoders
using the native LiteRT 2.4.0 CPU C API. No Python process, TensorFlow
installation, HTTP service, model downloads during inference, or GPU
inference. This is an independent C++ synthesis implementation, not a
port of the JUCE plugin.

## Using the filter

Add **LLL.sound Neural Timbre (DDSP)** to a microphone or other
monophonic source. The default runtime and model paths on Linux are
`~/.local/share/live-latent-lab-sound/ddsp/libLiteRt.so` and
`~/.local/share/live-latent-lab-sound/ddsp/models`. Environment
variables SOUND_DDSP_LIBRARY and SOUND_DDSP_MODELS can override the
defaults. The model folder contains all 11 instruments and the
feature extractor. Correct missing runtime/model paths in the
properties to retry an initial failed load; once successfully loaded,
asset paths are fixed for that filter instance. Save the new paths and
reload the scene to change them.

- Timbre A and B: Bassoon, Clarinet, Flute, Melodica, Saxophone, Sitar,
  Trombone, Trumpet, Tuba, Violin, Vowels. Switch live; all decoders are
  preloaded and the newly selected decoder's recurrent state is reset.
- Morph: interpolates actual harmonic amplitudes and noise
  magnitudes from the two selected decoders.
- Pitch: -24 to +24 semitones. Tracks the input's monophonic pitch.
- Harmonic gain and noise texture: independent 0–2 controls.
- Expression tracking: interpolates the model's loudness feature
  between a fixed reference and measured input expression.
- Silence gate: -80 to -20 dB. Suppresses spurious voiced synthesis
  on silence. This is a power gate, not a learned voicing confidence.
- Wet gain 0–3 and wet/dry mix 0–1, with a 10 ms mix ramp.

Wet audio is mono, duplicated to both channels of a stereo project.
The dry path preserves stereo, with a 200 ms fixed delay after
activation. During initial model loading or an initial model error,
the original signal passes through immediately. The transition to the
delayed pipeline initially inserts 200 ms of silence; it is not
seamless latency compensation. If a worker deadline is missed or
inference fails after activation, the filter stays at the fixed
delay and falls back to delayed dry audio; recreate the filter to
recover. No automatic dropped-hop or stale-wet substitution.
A/V sync compensation, disable/re-enable discontinuities, lengthy
hardware-clock drift and subjective quality remain to be qualified.
Do not use the first version as a zero-latency monitoring filter.

## Implementation

A 16 kHz feature extractor processes a 1024-sample (64 ms) causal
window, advanced in 320-sample (20 ms) hops. A 48-tap low-pass
resampler carries input history across hops at 44.1 or 48 kHz.
Two recurrent decoders return amplitude, 60 harmonic weights,
65 noise-band magnitudes and 512 recurrent state values each.
The synth normalizes harmonics below model-domain Nyquist, interpolates
frequency and harmonic amplitudes, maintains phase across hops,
and filters continuous white noise with a frequency-sampled
Hann-windowed FIR whose taps interpolate across hops. Linear streaming
upsampling returns exactly one host-rate hop. Output is bounded.
The output synthesis is an adaptation of the mathematical DDSP
architecture, not a claim of sample parity with DDSP-VST.

Model initialization, feature extraction, decoding, resampling,
and synthesis happen on the worker. The audio callback uses bounded
SPSC queues and preallocated dry delay storage only; no inference,
filesystem, mutex, or heap allocation. The read-only procedure
`sound_ddsp_status_v1` reports status, processed hops, detected input
pitch, power dB, last processing time in milliseconds and delay_ms.
Properties display status on opening, not a continuous live meter.

## Pinned assets and build

Reference implementation:
https://github.com/magenta/ddsp-vst/tree/f2996e97f9469f3956a6b8e9d2d9b50b6555e1e9
(Apache-2.0). The instrument pack is obtained from the URL in that
commit's scripts/download-models.sh:

`https://storage.googleapis.com/ddsp-vst/releases/DDSP-VST-Models.zip`

Observed archive SHA256:
`bbb0f4a61be86d8b52460b21210016945899d78f031b99a844a3771e9a2a5308`.
Extract only DDSP-VST-Models/*.tflite and its LICENSE, not __MACOSX
metadata. Fetch models/pitch-detection/extract_features_micro.tflite
from the pinned reference commit. The 12 models total 83,535,361 bytes.
Their expected individual sizes and SHA256 hashes are recorded in
`src/engines/ddsp_manifest.hpp` and verified before loading.
The original model pack license should accompany the assets.

Runtime: Linux x86_64 `libLiteRt.so` from
`ai-edge-litert==2.4.0` (Python 3.12 wheel, only the native shared
library used at runtime). Expected size 8,658,680 bytes and SHA256
`adb69308d4b2ff5d3116b3b7dd738802de3144f460dae2afb17790f031e24f0e`.
This is intentionally restricted to the qualified artifact, not an
arbitrary compatible ABI. A new runtime requires updating the
manifest after qualification. Keep the upstream license; no GPU
accelerator libraries are required. The C headers are from
https://github.com/google-ai-edge/LiteRT/tree/14b37ee5489894161548390f448c4a68c94f4ce5
(tag v2.4.0), including litert/c dependencies and the generated
litert/build_common/build_config.h (from build_config.h.in, both
DISABLE_GPU and DISABLE_NPU configure values 0 for the wheel).
Build the module with OpenSSL development headers and the pinned
C API include root. No weights or runtime binaries are in git.

```sh
cmake -S . -B build-ddsp -DSOUND_EXPERIMENTAL=ON \
  -DSOUND_EFFECTS=ddsp -DSOUND_DDSP_INCLUDE=/path/to/headers \
  -DOBS_PLUGIN_DESTINATION=bin/64bit -DOBS_DATA_DESTINATION=data
cmake --build build-ddsp -j3
cmake --install build-ddsp --prefix "$PWD/stage-ddsp"
```

For a combined build append ddsp to the explicitly selected effect
list from the expanded instructions, retaining the other runtime
options. The default quarantine and preset behavior are unchanged.

## Verification

`tests/verify-ddsp-models.py MODEL_DIRECTORY` in a separate
LiteRT 2.4.0 + NumPy environment verifies the actual model tensor
contracts, recurrent state, pitch tracking and all 11 control
decoders. Native C++ tests and actual OBS audio run with:

```sh
SOUND_DDSP_INCLUDE=/path/to/headers \
SOUND_DDSP_LIBRARY=/path/to/libLiteRt.so \
SOUND_DDSP_MODELS=/path/to/models \
SOUND_DDSP_STAGE=/path/to/stage-ddsp \
./tests/verify-ddsp.sh
```

On yuzu-mint on 2026-11-25, Python model qualification measured
feature p95 0.338 ms and decoder p95 0.088–0.107 ms on a single CPU
thread. Native pitch extraction plus two decoders averaged 0.515 ms.
The seven-effect staged module passed actual libobs audio at 44.1
and 48 kHz: 595 processed hops at each rate, last hop 1.136/1.092 ms
including resampling and synthesis, measured 220 Hz input pitch
219.885/219.890 Hz, live Violin/Flute morph and Trumpet switching,
octave transposition, nonzero filtered-noise-only output, stereo dry
preservation, zero invalid samples, zero settled silence output.
Missing-model dry passthrough and correction of the model path in
the same filter recovered actual synthesis (85 hops). Process
peak RSS of the full isolated libobs harness including all 11 models
was approximately 218 MiB; not a measured incremental model RSS.
These are tone and silence fixtures, not subjective voice or music
quality tests. The seven-effect registry and default empty quarantine
passed. The full expanded STT/TTS/SA3/stem suite was not rerun.
