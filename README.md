# Live Latent Lab: Sound

Native audio processing and inference tools for OBS Studio. This is a local
public-release candidate, not yet a published or redistribution-cleared release.

## Linux x86_64 candidate

All entries use the `LLL.sound` prefix in OBS.

| Filter or source | Function | Build profile |
| --- | --- | --- |
| Microphone DSP | Microphone character, filtering and audio shaping | Both |
| Native YAMNet (experimental) | Audio classification; dry stereo passthrough | Both |
| Buffered stem (experimental) | CUDA HTDemucs separation of one stereo stem, fixed 12-second delay | Expanded |
| STT | Select Whisper, Parakeet or streaming Nemotron | Expanded |
| TTS | Select Kokoro, Piper, Chatterbox, TikTok or Edge | Expanded |

### Speech and shared signals

Add one **LLL.sound TTS** source and select its speech backend in the
properties. Each engine remembers its own voice. There are no separate
per-engine source registrations.

| TTS backend | Execution |
| --- | --- |
| Kokoro, Piper | Local inference through the reader worker |
| Chatterbox Nano | Local inference through its worker |
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

The baseline profile includes only Mic DSP and YAMNet. The expanded
profile registers all five entries above. Other historical effects stay
quarantined.

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

For all five entries, use the
[expanded build instructions](docs/expanded-build.md), which describe
required runtime paths, models and acceptance tests.

## Verify

```sh
SOUND_MEDIAPIPE_LIB=/path/to/libmediapipe-1.2.3.so \
SOUND_YAMNET_MODEL=/path/to/yamnet.tflite \
./tests/verify-public-linux.sh
./tests/verify-quarantine.sh
```

The test builds a new module and exercises actual OBS registration, DSP,
classification, missing/invalid model handling, stale expiry and 44.1/48 kHz
stereo passthrough in isolated libobs. It requires xvfb-run. Neither test
edits normal OBS scenes or installs to the user profile.

The latest expanded build passed the five-entry registration check,
real five-engine TTS switching and playback, cancellation/recovery and
the independent signal-client test. Earlier full feature-group tests
passed individually, with an intermittent Parakeet lifecycle failure at
44.1 kHz. The full STT/stem suite has not been rerun on the latest build.
See [release status](RELEASE_STATUS.md); the expanded profile remains opt-in.

## License and distribution

See LICENSE (GPL-2.0). Model weights and third-party runtime distribution
are separate terms. No weights are included. The candidate is not yet
approved for external publication; dependency provenance, fresh-machine
testing and packaging remain open.
