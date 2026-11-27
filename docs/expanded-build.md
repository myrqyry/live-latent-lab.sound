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

### Shared HTDemucs model controls

Launch the existing local sidecar with the model-specific options
below. These control the shared worker, not each OBS filter.

```sh
python tools/live_stem_sidecar.py --device cuda --shifts 0 \
  --overlap 0.1 --segment-seconds 4 --precision fp32
```

Use the installed stem-splitter Python environment and cached model
directory. `--device auto` is the default; explicit CPU or CUDA
selection fails if the runtime cannot honor it. CPU is not qualified
for live deadlines. Shifts 0–2, overlap 0.05–0.5 and segment seconds
1–7 are bounded choices. Omit segment length for model default;
defaults are one shift and 0.25 overlap. A synthetic warmup runs
before the socket accepts audio. High quality settings can exceed
the fixed four-second inference allowance; missed deadlines mute.
The filter now starts a fresh capture generation after a queue fault,
socket failure or deadline miss. Each recovered stream has its own
12-second silent prefill and discards the old queued audio. It never
substitutes unseparated music for a stem.

The existing worker is external and user-owned: the OBS filter
reconnects to its socket, rather than spawning or terminating it.
Three automatic attempts use 250/500/1000 ms backoff; four successful
eight-second windows replenish the budget. A missing endpoint
remains silent while attempting recovery. Changes to the socket or
selected stem reload live and start a new silent prefill. **Retry
worker** explicitly resets the budget after exhaustion. The read-only
`sound_stem_live_v1` procedure reports status (buffering, ready, error),
cumulative successful windows, automatic retries and delay_ms. Ready
means a valid window has been queued, not necessarily that the silent
prefill has elapsed. The delay is counted in received samples; this is
not hardware-clock-drift correction.

A request is cancellable on the worker thread if its generation
becomes obsolete, with nonblocking socket I/O, 20 ms poll intervals
and one 12-second transfer deadline. The audio deadline still governs
output: the longer transfer cap is not permission to play late stems.
Closing the socket does not cancel an ongoing model call inside the
external sidecar. The next request may contend with that old work.
No dry bypass, shared multistem fanout or in-memory inference has been
added by this recovery change.
The worker CLI can be changed before launching/restarting it.
A global OBS graphical worker manager remains future work.

The existing verification runner accepts STEM_VERIFY_DEVICE,
STEM_VERIFY_SHIFTS, STEM_VERIFY_OVERLAP and STEM_VERIFY_SEGMENT
to check a selected configuration using its isolated real model
test. For example, with the required fixture/environment configured:

```sh
STEM_VERIFY_DEVICE=cuda STEM_VERIFY_SHIFTS=0 STEM_VERIFY_OVERLAP=0.1 \
  STEM_VERIFY_SEGMENT=4 ./tests/verify-stem-live.sh --with-model
```


For a deterministic real-libobs lifecycle test without the model:
```sh
SOUND_STEM_STAGE=/path/to/staged-sound SOUND_RT_PROBE=1 \
  bash tests/verify-stem-recovery-obs.sh
```
This uses a test-only protocol peer, not HTDemucs. The real-model runner
adds strict recovery checks without changing the audio deadline.
See RELEASE_STATUS.md for the distinction between verified transport
and outstanding sustained model qualification.
