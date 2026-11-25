# Native RVC voice conversion (experimental)

The opt-in `rvc` effect now builds a native-worker filter instead of
the quarantined ONNX adapter. The historical source ID
`stts_rvc_filter` is retained. A saved ONNX voice selection is not
silently replaced with a different voice: explicitly select the native
package and reload the scene to use the new backend. No ONNX dependency
is linked for this effect.

## Build and local assets

See [native qualification](native-rvc-qualification.md) for model and
upstream release provenance, the initial CUDA timing and paced SOLA
test. The production candidate uses the pinned audio.cpp C API,
not a generic HTTP server. Build a separate narrow runtime from
commit db1869d0d53c7839b79cf7c1a16d36cc20102307:

```sh
cmake -S . -B build-rvc -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=rvc \
  -DAUDIOCPP_BUILD_C_API=ON -DENGINE_ENABLE_CUDA=ON \
  -DENGINE_ENABLE_CUDA_GRAPHS=OFF -DENGINE_ENABLE_VULKAN=OFF \
  -DGGML_CUDA_NCCL=OFF -DGGML_CUDA_FA=ON \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-12 \
  -DCMAKE_CUDA_ARCHITECTURES=75
cmake --build build-rvc --target audiocpp -j8
```

These compiler/architecture settings are for the qualified Turing host.
Do not disable FlashAttention merely to shorten the build. Build the
private helper in Sound, with the pinned upstream header:

```sh
c++ -std=c++17 -O2 -I src -I /path/to/audio.cpp/include \
  tools/rvc-native-worker.cpp -ldl -lcrypto -o rvc-native-worker
cmake -S . -B build-sound-rvc -DSOUND_EXPERIMENTAL=ON \
  -DSOUND_EFFECTS=rvc -DOBS_PLUGIN_DESTINATION=bin/64bit \
  -DOBS_DATA_DESTINATION=data
cmake --build build-sound-rvc -j3
```

Default local paths are under
`~/.local/share/live-latent-lab-sound/rvc/`: rvc-native-worker,
libaudiocpp.so.0.1.0, and rvc-f16.gguf. Keep upstream licenses and
record runtime/library hashes. The helper verifies the pinned model
byte count and SHA-256, checks C ABI major version, and explicitly
selects CUDA device 0 (CPU is available only to the diagnostic
helper). It checks for 4000 MiB free on startup and 1024 MiB before
and after conversion; this is a point-in-time check, not a GPU
reservation. No implicit CPU fallback. No user-supplied model
or executable should be trusted merely because the picker accepts a
path. Individual voice model rights remain unverified; use only
voices you are entitled to use.

## Behavior and limitations

The first version uses only the packaged default voice, F32 runtime
weights, and disables retrieval. Live pitch shift (-24 to +24
semitones) and wet/dry mix are exposed. Voice selection, custom
checkpoint and FAISS retrieval UI remain future work.
A single private worker owns the RVC CUDA session; additional
instances fail closed. The binary protocol carries 16000 mono
float samples per request, an output rate, pitch and sequence.
The child is spawned with an inherited socketpair, no listener.
The worker is cancelled by closing/reaping its own child process;
the audio callback never waits on the worker.

Input mono or stereo at 44.1/48 kHz is converted to one-second
16 kHz windows (250 ms left context; 500 ms hop; 250 ms right
context), with windowed-sinc anti-aliasing. The output at the host
rate is cropped starting at 240 ms, aligned and crossfaded with
the existing SOLA stitcher, and emitted in exact half-second
hops. Wet is mono (duplicated to stereo); dry preserves stereo.
After warming, the callback starts a 1500 ms fixed delay with
initial silence. Initial loading or missing assets pass through
undelayed dry. Once activated, a queue overrun, missed deadline,
timestamp discontinuity, or failed conversion permanently switches
that instance to delayed dry. Recreate the filter to retry; runtime
and model paths are read at creation. An input-idle timeout of
30 seconds frees the worker. Changes in latency when activating,
bypassing or recreating the filter are not seamless. No A/V sync
compensation or long-run clock-drift correction. The status proc
`sound_rvc_native_v1` reports readiness, completed hops, most
recent processing ms and fixed delay; the properties status is
a snapshot when opened.

## Acceptance

`tests/verify-rvc-safety.sh` is a CPU-only negative test, with actual
libobs registration and missing-helper dry passthrough. It does not
claim working voice conversion. For native positive verification:

```sh
SOUND_RVC_HEADER_ROOT=/path/to/audio.cpp/include \
SOUND_RVC_LIBRARY=/path/to/libaudiocpp.so.0.1.0 \
SOUND_RVC_MODEL=/path/to/rvc-f16.gguf \
SOUND_RVC_FIXTURE=/path/to/synthetic-speech-16k.wav \
SOUND_RVC_STAGE=/path/to/staged-sound \
./tests/verify-rvc-native-filter.sh
```

The native test checks the real private C API protocol, invalid
request rejection, distinct pitch-shifted output, both output
rates, finite samples, and continuous OBS stereo dry/wet processing.
No user mic capture or online model service.

## Actual native acceptance (2026-11-25)

Built the narrow CUDA C ABI at the pinned commit with CUDA 12.9,
SM75, FlashAttention on, CUDA graphs off, no NCCL dependency.
The library is 64 MiB; SHA256
e485ca410b4ea7f503db58b4ef6bbbf5b43b7b60c145bc4f8d0c10e180b7f977.
Real private conversion (including input/output PCM transport):
4957.3 ms cold (including checksum and model load), 306.6 ms warm
at 48 kHz, 310.0 ms warm at 44.1 kHz. The same resident process
rejected out-of-range pitch and accepted the subsequent request.
Output had 47040 / 43218 frames respectively; finite and nonempty.
The combined staged Sound module SHA256
5ff194cb0d8b72682d92b606e2b9dd983e8f51738a90371a0327d5e5110ef6aa
passed actual nine-second libobs streams at 48 and 44.1 kHz:
16 completed hops each, last processing time 300.5 / 305.7 ms,
wet RMS 0.04132, wet stereo difference zero, dry stereo difference
0.02687, no invalid samples or terminal pipeline faults. These
checks establish absence of output starvation in the test, not
per-hop deadline statistics or measured end-to-end latency.
Negative missing-helper and saved ONNX voice checks passed,
as did default quarantine and seven-effect registry (stem_live,
yamnet, tts, stt, diffusion, ddsp, rvc). Normal destruction during
an in-flight request can log a worker-unavailable warning; the
child is cancelled and reaped. No subjective audition, extended
soak, actual microphone, live pitch accuracy, child crash recovery
inside an active filter, arbitrary contention or full expanded
regression suite qualification. Only default packaged voice is
exposed. The runtime and model are present at the default paths.
The tested module is staged, not installed: the existing installed
module predates the separate removal of deterministic mic
processing in commit 5df496d.
