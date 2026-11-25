# Native Kokoro CUDA reader

The existing TTS source offers a separate **Kokoro (native GGUF CUDA)**
backend. ONNX remains the default. Native and ONNX retain distinct
voice, speed and model leases. The native choice supports the 28
English voice IDs already exposed by Sound, voice-matched pronunciation
and speed 0.5–2. The GGUF has other voice packs, but those are not
qualified by this integration. ONNX-only phoneme input, pronunciation
override, batch trim and pause controls are not exposed on native.
Unsupported options are rejected rather than ignored.

The existing owner-only Unix HTTP socket remains the OBS interface.
The reader launches one child on demand with a private inherited
socketpair and a bounded packet protocol. Only that child loads the
audio.cpp C ABI, validates the pinned GGUF's size and checksum,
creates a CUDA device-0 session and synthesizes mono 24 kHz PCM16 WAV.
No HTTP listener, NCCL, cloud fallback or new Python packages.
The native session is retained until the existing lease/TTL policy
evicts it. Errors terminate the child; another explicit request can
start a fresh worker. OBS cancel suppresses obsolete playback;
the bounded in-flight native request may still finish on the worker
(up to the worker's 40-second socket timeout). No claim of immediate
native kernel interruption.

## Runtime

Build from clean audio.cpp commit
`db1869d0d53c7839b79cf7c1a16d36cc20102307`
with its vendored GGML, using the following options (Linux Mint,
CUDA 12.9, GCC 12, Turing compute capability 7.5):
```sh
cmake -S . -B build-kokoro -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=kokoro_tts \
  -DAUDIOCPP_BUILD_C_API=ON -DENGINE_ENABLE_CUDA=ON \
  -DENGINE_ENABLE_CUDA_GRAPHS=OFF -DENGINE_ENABLE_VULKAN=OFF \
  -DGGML_CUDA_NCCL=OFF -DGGML_CUDA_FA=OFF \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-12 \
  -DCMAKE_CUDA_ARCHITECTURES=75
cmake --build build-kokoro --target audiocpp -j8
```
Select the appropriate compiler and architecture on other hosts.
Keep upstream LICENSE and C header with the library. Verify dependency
resolution with ldd; the qualified 49 MiB shared library has no NCCL
dependency. See [qualification](native-kokoro-qualification.md) for
model provenance. No binaries or model weights are included in Sound.

Enable the backend by adding both arguments to the existing reader:
```sh
--native-kokoro-library /path/to/libaudiocpp.so.0.1.0 \
--native-kokoro-model /path/to/kokoro-82m-q8_0.gguf
```
No configuration is needed for the ONNX backend. If the native worker
is not configured, requests fail explicitly.

## Acceptance

Run `python3 tests/verify-native-kokoro.py` for request validation.
With the two paths as arguments, it also checks real CUDA synthesis,
voice/speed changes, child reuse, dead-worker rejection, recovery and
eviction. Run `tests/verify-native-kokoro-obs.sh` with
SOUND_KOKORO_LIBRARY, SOUND_KOKORO_MODEL, SOUND_READER_PYTHON,
SOUND_READER_ASSETS and SOUND_STAGE for isolated actual libobs/reader
playback, speed, cancel, recovery, CPU reader regression, lease eviction,
and unsupported-option rejection. Existing service and scenes are not
modified by the test.
