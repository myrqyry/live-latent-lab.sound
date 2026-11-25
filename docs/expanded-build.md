# Expanded Linux candidate

The expanded profile selects
stem_live;yamnet;tts;stt. The smaller public-linux preset has no speech
runtime or GPU dependency and remains available. The expanded profile exposes one TTS source, with seven speech backends
selectable inside it (including optional native Kokoro CUDA and native
Turbo CPU). There are no separate per-backend registrations.
Audio Diffusion and DDSP are additional explicit opt-in effects,
not part of the four-entry preset.

Build prerequisites: C++17, CMake 3.28+, Ninja, libobs and frontend headers,
pkg-config, libcurl, FFmpeg development libraries (libavformat, libavcodec, libavutil, libswresample), and separately acquired matched native speech runtimes.
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

### Nemotron compute device (CUDA / CPU)

Nemotron's pinned `transcribe.cpp` backend now selects a GPU automatically
when one is compiled in and available. It falls back to CPU in Auto mode;
the **CUDA** selection requires actual CUDA rather than silently using CPU.
The OBS Nemotron filter exposes **Auto**, **CUDA**, and **CPU** and logs
the backend it actually loaded. CPU threads can be set up to the machine's
reported hardware concurrency, rather than a fixed 12-thread cap.

To build the native runtime from the existing pinned `transcribe.cpp`
source, rather than using NVIDIA NIM:

```sh
cmake -S tools/nemotron-runtime -B build-nemotron-runtime \
  -DTRANSCRIBE_SOURCE=/path/to/pinned/transcribe.cpp \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-nemotron-runtime --target sound-nemotron-runtime -j 2
```

The runtime build detects an available CUDA compiler and builds CUDA
kernels by default; on systems without one it builds CPU support. Use
`-DSOUND_NEMOTRON_CUDA=OFF` to produce a CPU-only build. Optionally set
`-DCMAKE_CUDA_ARCHITECTURES=native` (or a specific supported architecture)
when targeting the current GPU, and `-DCMAKE_CUDA_HOST_COMPILER` when your
CUDA toolkit requires a specific host compiler. This separate runtime must
still be packaged with its required license notices for staging; no model
weights are included. Test the actual selected backend and captions in OBS
before replacing a working runtime.

## Unified STT filter

Use the existing **LLL.sound STT** filter. Choose the recognition engine,
then its model and compute device. Caption destination, energy gate, phrase
timing, caption hold and muted-audio policy occupy the same positions for
all supported engines. Engine-specific options follow the shared controls.
The model and device widgets use the existing per-engine saved keys so a
switch restores that engine's previous selection rather than overwriting it.

A worker-owned recognition session exposes common open, close, device,
reset, finalize and cancellation operations. Nemotron and the optional [Zipformer CPU runtime](zipformer-stt.md)
provide incremental transcription; Whisper and Parakeet publish
completed phrases. Cancellation checks discard stale results from non-interruptible
native calls; they do not promise immediate native cancellation. The
existing capture, caption and versioned transcript-signal contracts remain
unchanged. Settings belonging to an inactive engine do not reload the
active model. Disabling the filter still unloads the model.

Auto device selection is runtime-dependent, not a guarantee that a GPU
backend exists. Explicit GPU requests do not intentionally retry on CPU.
A CUDA-enabled Nemotron runtime has been verified separately; Whisper's
packaged CPU runtime remains CPU-only. The MediaPipe delegate is not a
CUDA selector. Inspect the actual runtime log and test your model/device.

Model paths remain manual. A discovered model catalog, live UI telemetry,
retention policy and improved voice activity detection are separate work.

## Local reader TTS device selection

The existing local reader worker accepts `--device auto|cpu|cuda`.
Auto attempts CUDA only when the installed ONNX Runtime advertises
CUDAExecutionProvider, verifies its presence in the created session
and falls back to CPU on selection failure. CPU explicitly creates
CPU-only sessions. Explicit CUDA requires a CUDA provider and fails
if unavailable instead of reporting a CPU result as GPU execution.
ONNX may still partition unsupported operations onto CPU; provider
registration is not proof of per-operation acceleration. Both Piper
and Kokoro log their session provider choices. Chatterbox retains its
existing CUDA-specific worker and is not controlled by this reader
policy. This ONNX device policy does not control native Kokoro CUDA or native Turbo CPU.

Run `python3 tests/verify-tts-device.py` for policy coverage.
Actual local Piper and Kokoro speech generation and Unix HTTP requests
were tested using the existing reader runtime, which advertises only
CPU (and Azure) providers. CPU is the qualified real ONNX device on this
machine; ONNX CUDA branches have deterministic policy coverage only.
Separate [native Kokoro CUDA](native-kokoro.md) and
[native Turbo CPU](native-turbo.md) backends use qualified audio.cpp
libraries and explicit reader arguments. The local combined build
was installed and native Turbo tested through the running reader
on 2026-11-25. Nano remains a separate service.

## Reproduce the full native acceptance check

The native acceptance runner requires explicit external asset paths
rather than machine-specific model defaults. A matching libobs and OBS
text-source test environment is still needed. Configure the three
variables above, plus:

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
integration tests. TikTok uses native HTTPS and in-process MP3 decoding; no localhost proxy or FFmpeg executable is required at runtime. The Edge and TikTok checks send generated test text to their
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
