# Expanded Linux candidate

The expanded profile uses the original native implementation. It selects
mic;stem_live;yamnet;tts;stt. The smaller public-linux preset has no speech
runtime or GPU dependency and remains available.

Build prerequisites: C++17, CMake 3.28+, Ninja, libobs and frontend headers,
pkg-config, libcurl, and separately acquired matched native speech runtimes.
Set the three CMake inputs as environment variables:

```sh
export SOUND_STT_WHISPER_INCLUDE=/path/to/matched/whisper/include
export SOUND_STT_RUNTIME_DIR=/path/to/whisper/runtime
export SOUND_STT_NEMOTRON_RUNTIME_DIR=/path/to/nemotron/runtime
cmake --preset public-linux-expanded
cmake --build --preset public-linux-expanded
cmake --install build-public-expanded --prefix "$PWD/stage-expanded"
```

The staged plugin includes speech runtime libraries selected at build time.
Do not distribute those binaries until their licenses have been cleared.
Models are separate. Set the paths in OBS; setup of external runtimes and
workers on a clean machine is still unqualified.

## Reproduce the full native acceptance check

The existing private development test was adapted to require explicit
paths instead of relying on the developer workstation. Configure the
three variables above, plus:

| Variable | Input |
| --- | --- |
| SOUND_MEDIAPIPE_LIB | Compatible native MediaPipe 1.2.3 runtime |
| SOUND_YAMNET_MODEL | Compatible TFLite classifier |
| SOUND_STT_MODEL | Compatible Whisper model |
| SOUND_STT_PARAKEET_MODEL | Qualified stateful Parakeet LiteRT-LM |
| SOUND_STT_NEMOTRON_MODEL | Qualified streaming Nemotron GGUF |
| SOUND_STT_FIXTURE | Licensed speech WAV fixture matching the original JFK test |
| STEM_SPLITTER_PYTHON | Python executable with stem worker dependencies and cached HTDemucs |
| STEM_VERIFY_SOURCE_WAV | Licensed 44.1 kHz stereo fixture, at least 16 seconds |

Use the existing TTS workers in a separate XDG_RUNTIME_DIR for the
integration tests. The TikTok test uses the configured localhost proxy on
port 8790. The Edge and TikTok checks send generated test text to their
online speech services, not live microphone input. Local-only users can
omit the full online qualification until those services are configured.

```sh
./tests/verify-stem-live.sh --combined-stt
```

This test builds and stages its own module and exercises isolated libobs.
It does not install into the normal OBS profile. It is not a clean-machine
runtime installer, a sustained production performance test, or proof of
redistribution rights. The existing test still depends on matching OBS
development and text-source components being installed.
