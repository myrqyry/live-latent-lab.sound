# Native Stable Audio 3 qualification — 2026-11-24

Research and real local inference checkpoint, not an enabled OBS effect.
Upstream [sa3.cpp](https://github.com/betweentwomidnights/sa3.cpp) is MIT
licensed; model weights have separate terms and remain external.
Pinned source: 28ebf5f5337ba6f36966db76d16cad1fc5ed2b70.
Pinned GGML: a373144b892cb1ef7f0c3453efd558caebb0a891.

## Local build

Linux Mint, RTX 2070 SUPER (SM 75, 8 GiB nominal), CUDA 12.9,
GCC 12.4, CMake 3.28, Ninja. Build the upstream shared library
and its C ABI contract test with:

```sh
cmake -S /path/to/sa3.cpp -B /path/to/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSA3_CUDA=ON -DSA3_PRIVATE_GGML=ON \
  -DSA3_BUILD_TOOLS=ON -DCMAKE_CUDA_ARCHITECTURES=75 \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-12 \
  -DGGML_CUDA_NCCL=OFF -DGGML_CUDA_FA=OFF
cmake --build /path/to/build --target sa3_shared sa3-lib-v1-contract -j 6
ctest --test-dir /path/to/build -R '^sa3-lib-v1-contract$' --output-on-failure
```

Stage libsa3 and its four private GGML shared libraries in one directory,
preserve SONAME aliases, set the staged libraries' RUNPATH to $ORIGIN,
and retain upstream/third-party license notices. The qualified runtime is
165 MiB; its dependencies resolve without the build directory or a global
GGML install. This is a machine-specific CUDA build, not a distributable
Linux package.

Five small-music model files (Q4_K_M DiT, F16 SAME-S, F16 T5Gemma and
the two supporting files) were downloaded to external storage and checked
against the exact source revisions and SHA256 in
tests/fixtures/sa3-small-music-gguf.json. Total approximately 1.1 GiB.
The model licenses/notice are stored with the weights.

## Reproduce the real native test

```sh
SOUND_SA3_SOURCE=/path/to/pinned/checkout \
SOUND_SA3_LIBRARY=/path/to/staged/libsa3.so \
SOUND_SA3_MODELS=/path/to/gguf/models \
SOUND_SA3_OUTPUT=/path/to/new-output.wav \
./tests/verify-sa3-native.sh
```

The probe dynamically loads the public ABI, uses the real model, verifies
finite non-silent stereo planar float PCM at 44.1 kHz, exact frame count,
continuation, transformation, immediate cancellation and invalid duration,
and renders a 12-second WAV. The wrapper verifies the five model hashes,
requires the real CUDA0 backend log, and rejects a build-directory
dependency or preexisting WAV. Upstream device selection can fall back to
CPU; requesting CUDA alone does not prove GPU usage.

## Observed results

Upstream model-free ABI contract passed. Two separate executions of the
corrected model-backed probe passed on the exact staged runtime. The first
measured (warm filesystem cache, eight steps, fixed seeds, frugal residency):

| Operation | Wall time |
|---|---:|
| Context create | 0.799 s |
| Generate 12 seconds | 0.930 s |
| Extend 12 seconds by 6 seconds | 1.066 s |
| Extend with prefix-preserving settings | 1.056 s |
| Transform 12 seconds | 0.961 s |
| Generate 30 seconds | 1.104 s |
| Generate 60 seconds | 1.677 s |

Peak sampled process VRAM 1112 MiB (100 ms sampling, not an absolute peak).
The independently rerun wrapper measured 1.645 s for the 60-second render.
The initial standalone probe failed its overly strict default prefix
equality expectation: upstream splices the source with gain matching and
then normalizes/limits the entire result. Comparing against the documented
postprocessing formula produced zero measured difference on the first
second of the restored prefix. Disabling gain match, peak normalization
and limiter for a continuation produced zero measured difference against
the original first second. This is a useful control for future continuous
playout; it does not prove perceptual seamlessness or unlimited generation.
Raw output requires host-side finite/range validation and appropriate
processing on the newly generated suffix, without changing emitted audio.

## Integration boundary

The next narrow feature is an opt-in native audio diffusion source using
the existing C ABI on a dedicated worker, clocked stereo playout, frugal
residency, progress/cancel, and bounded audio buffers. Start with explicit
generation and replay, then qualify continuation and adaptive buffered
playout. Do not run inference on the OBS callback or render thread.
Separate library names avoid filename collisions but do not establish ELF
symbol isolation or coexistence with loaded Whisper/Nemotron, Sight or
other GPU plugins; real same-process and concurrent-load tests are still
required. Small-SFX, longer continuous playback, mid-render cancellation,
other GPUs, OBS capture/playout and installation are not yet qualified.

No Sound runtime code, default effect registration, OBS installation,
scene, worker service or remote repository was changed for this checkpoint.
