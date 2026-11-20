# Live Latent Lab: Sound

Native audio processing and inference tools for OBS Studio. This is a local
public-release candidate, not yet a published or redistribution-cleared release.

## First candidate: Linux x86_64

- Microphone DSP: microphone character, filtering and audio shaping.
- YAMNet: asynchronous audio classification; original stereo audio passes
  through unchanged. Requires a compatible MediaPipe 1.2.3 runtime and
  YAMNet model supplied by the user.

The separate [expanded Linux profile](docs/expanded-build.md) includes
buffered HTDemucs stems, unified five-backend TTS, Whisper/Parakeet/Nemotron
STT, speech relay and cross-plugin signals. Each group has passing real
OBS tests on one exact module; an intermittent Parakeet 44.1 kHz lifecycle
test prevents a clean combined qualification claim. See
[release status](RELEASE_STATUS.md). Keep this profile opt-in.
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
until it has been backed up and checked for scene compatibility.

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

## License and distribution

See LICENSE (GPL-2.0). Model weights and third-party runtime distribution
are separate terms. No weights are included. The candidate is not yet
approved for external publication; dependency provenance, fresh-machine
testing and packaging remain open.
