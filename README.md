# Live Latent Lab: Sound

Native audio processing and inference tools for OBS Studio.

**Source:** [live-latent-lab.sound](https://github.com/myrqyry/live-latent-lab.sound)
(currently private). This is a curated, separately rooted release-candidate
repository, not the historical development repository. There are currently
**no GitHub releases or redistribution-cleared installers**; building locally
does not grant permission to redistribute third-party runtimes or models.

**Start here:** The `public-linux` preset below builds the smaller YAMNet candidate. For STT, TTS and buffered stems, follow the
[expanded Linux instructions](docs/expanded-build.md). See the
[release status](RELEASE_STATUS.md) for observed tests and unresolved issues.

## Linux x86_64 candidate

All entries use the `LLL.sound` prefix in OBS.

| Filter or source | Function | Build profile |
| --- | --- | --- |
| Native YAMNet (experimental) | Audio classification; dry stereo passthrough | Both |
| Buffered stem (experimental) | CUDA HTDemucs separation of one stereo stem, fixed 12-second delay | Expanded |
| STT | Select Whisper, Parakeet or streaming Nemotron; optional [Zipformer CPU](docs/zipformer-stt.md); rolling captions | Expanded |
| TTS | Select Kokoro ONNX/native CUDA, Piper, Chatterbox Nano/native Turbo CPU, TikTok or Edge | Expanded |
| [Audio Diffusion](docs/audio-diffusion.md) | Local Stable Audio 3 music/SFX, continuous playback, capture and transformation, prompt/LoRA automation, optional resident SAME-S morph/texture and overlapping live wet output | Explicit opt-in |
| [Neural Timbre (DDSP)](docs/ddsp.md) | CPU-native monophonic timbre transfer, 11 instruments, live morph and independent harmonic/noise synthesis | Explicit opt-in |

### Speech and shared signals

Add one **LLL.sound TTS** source and select its speech backend in the
properties. Each engine remembers its own voice. There are no separate
per-engine source registrations.

| TTS backend | Execution |
| --- | --- |
| Kokoro, Piper | Local ONNX inference through the reader worker |
| [Kokoro native GGUF CUDA](docs/native-kokoro.md) | Optional isolated native worker with the same local reader socket |
| Chatterbox Nano | Local inference through its worker |
| [Chatterbox Turbo native GGUF CPU](docs/native-turbo.md) | Optional CPU-only built-in-voice native worker with expressive text tags |
| TikTok | Third-party Ottsy cloud synthesis; native HTTPS and in-process MP3 decoding |
| Edge | Online speech service through its worker |

TikTok does not require a localhost Python proxy or an FFmpeg executable;
the TTS build links FFmpeg shared libraries. It still sends text to an
external service. Local speech workers require separate setup.

STT can publish captions and speech events. The expanded profile also
supports STT-to-TTS relay and [shared audio/speech signals](docs/sound-signal-interop-v1.md).
Sending recognized speech to cloud TTS requires explicit opt-in.

YAMNet requires a separately supplied compatible MediaPipe 1.2.3 runtime
and classification model.

The baseline profile includes only YAMNet. Deterministic microphone modeling now lives in [HyperDSP](https://github.com/myrqyry/hyperdsp). The expanded
profile registers the four baseline/expanded entries above. Audio Diffusion
requires a separately qualified SA3 runtime and the explicit diffusion gate.
DDSP requires its separately qualified LiteRT runtime/models and ddsp gate.
Other historical effects stay quarantined.

## Build and stage

Requirements: CMake 3.28+, Ninja, C++17, pkg-config, compatible libobs and
OBS frontend development files. Target OBS 32 on Linux; Windows and macOS
are not qualified. No model downloads or system installation are automatic.

```sh
cmake --preset public-linux
cmake --build --preset public-linux
cmake --install build-public --prefix "$PWD/stage"
```

Close OBS before installing. The staged package contains the plugin under
bin/64bit and locale assets under data. Do not overwrite a working plugin
until it has been backed up. The public candidate does not register
obsolete development-only speech sources.

For the four-entry expanded preset, use the
[expanded build instructions](docs/expanded-build.md). To add the
two optional neural effects, see [Audio Diffusion](docs/audio-diffusion.md)
and [Neural Timbre](docs/ddsp.md) for their required runtimes,
models, explicit build gates and acceptance tests.

## Verify

```sh
SOUND_MEDIAPIPE_LIB=/path/to/libmediapipe-1.2.3.so \
SOUND_YAMNET_MODEL=/path/to/yamnet.tflite \
./tests/verify-public-linux.sh
./tests/verify-quarantine.sh
```

The test builds a new module and exercises actual OBS registration,
classification, missing/invalid model handling, stale expiry and 44.1/48 kHz
stereo passthrough in isolated libobs. It requires xvfb-run. Neither test
edits normal OBS scenes or installs to the user profile.

The locally installed seven-effect build (2026-11-25) includes the
expanded profile plus Audio Diffusion and Neural Timbre (DDSP).
Native Kokoro CUDA and CPU-only Chatterbox Turbo are additional
backends within the single TTS source, for seven selectable engines.
The installed module passed seven-effect registration, actual DDSP
audio at 48 kHz, and Turbo speech playback, controls, cancellation
and recovery. The staged DDSP build also passed 44.1 kHz and
missing-model recovery. The complete expanded STT/stem/TTS/SA3 suite
has not been rerun on this build. Earlier Parakeet testing had an
intermittent 44.1 kHz lifecycle failure. See
[release status](RELEASE_STATUS.md) for evidence and limitations.
The previous seven-effect installation described above predates removal of microphone DSP. Current mic-free presets exclude it; diffusion and
DDSP require explicit opt-in gates. No release binaries or model
weights are published here.

## License and distribution

See LICENSE (GPL-2.0). Model weights and third-party runtime distribution
are separate terms. No weights are included. The candidate is not yet
approved for external publication; dependency provenance, fresh-machine
testing and packaging remain open.
