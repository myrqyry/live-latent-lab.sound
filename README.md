# Live Latent Lab: Sound

Native audio processing and inference tools for OBS Studio.

> **v0.3.0 experimental source-preview candidate.** Linux x86_64 / OBS 32.2.0 is the tested target. No production installers or bundled model weights. The baseline and expanded builds are deliberately separate.

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
| [Voice Conversion (RVC CUDA)](docs/native-rvc.md) | Native GGUF voice conversion, custom RVC checkpoints, optional FAISS retrieval, pitch shift and SOLA | Explicit opt-in, staged |

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
and classification model. Follow the [verified asset setup](docs/yamnet-setup.md).

The baseline profile includes only YAMNet. Deterministic microphone modeling now lives in [HyperDSP](https://github.com/myrqyry/hyperdsp). The expanded
profile registers the four baseline/expanded entries above. Audio Diffusion
requires a separately qualified SA3 runtime and the explicit diffusion gate.
DDSP requires its separately qualified LiteRT runtime/models and ddsp gate.
Other historical effects stay quarantined.

## Plugin preferences

Enable the optional native Settings integration with
`-DSOUND_SETTINGS_PAGE=ON` (the Audio Diffusion dock build enables it by
default unless explicitly disabled). In OBS, open **Settings → Plugins →
LLL.sound** to select starting folders for model and runtime file pickers
and access backend-specific model, setup and licensing resources.
Apply/OK saves preferences; Cancel discards unapplied changes.
Existing sources retain their own selected models and devices.
See [plugin settings integration](docs/plugin-settings.md).

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
[expanded build instructions](docs/expanded-build.md). To add the three opt-in effects, see [Audio Diffusion](docs/audio-diffusion.md),
[Neural Timbre](docs/ddsp.md), and [Voice Conversion](docs/native-rvc.md)
for their runtimes, models, build gates and acceptance tests.

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

### Qualification scope

The current seven-effect profile contains YAMNet, buffered stems,
STT, TTS, Audio Diffusion, DDSP and native RVC. Microphone modeling has
moved to HyperDSP. Individual real OBS audio tests at both 44.1/48 kHz
cover multiple backends, but the latest combined acceptance suite has
not passed as one uninterrupted run. Parakeet has an unresolved
intermittent 44.1 kHz lifecycle failure; the SA3 lifecycle also required
a retry. The latest seven-effect module has been installed locally and verified with isolated OBS registration, YAMNet, DDSP and native CUDA RVC at 48 kHz. This is local qualification, not a distributed installer.

[Release evidence and history](RELEASE_STATUS.md) · [Model settings audit](docs/model-settings-audit.md)
· [Build checks](https://github.com/myrqyry/live-latent-lab.sound/actions)

The shared Linux CI driver builds the small YAMNet profile and verifies
registration, quarantine, ring buffer, STT configuration, public links, and the optional settings bridge and actual property-picker defaults.
It uploads a model-free test archive and corresponding source, not an
expanded installer. Actual classification requires the external assets above.

## License and distribution

GPL-3.0-or-later for original project software. See [LICENSE](LICENSE), [NOTICE](NOTICE.md) and the [dependency/distribution inventory](docs/distribution.md). External model and runtime terms are separate. Full expanded redistribution and clean-machine provisioning remain unqualified.

See the [model settings coverage audit](docs/model-settings-audit.md) for backend-specific controls, fixed runtime contracts and remaining gaps.
