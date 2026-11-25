# Native Chatterbox Turbo CPU

A separate **Chatterbox Turbo (native GGUF CPU)** backend uses the
existing private reader socket and the same resident child-process
transport as native Kokoro. It supports its embedded default English
voice, expressive text tags, temperature, top-k, top-p and repetition
penalty. It deliberately does not offer reference audio or reference
loudness normalization. The qualified upstream Turbo package excludes
the voice encoder/tokenizer components needed for custom reference
conditioning. Use Chatterbox Nano for cloning; unsupported native
options are rejected rather than silently ignored.

Native Kokoro and Turbo have separate sessions, model leases and
source settings. CPU ONNX and Nano paths remain available. Native Turbo
explicitly
selects the CPU backend and hides CUDA devices in its private child.
It never silently switches an intended CUDA request to CPU. No cloud
fallback. Cancel invalidates obsolete playback, but the bounded
worker request may still finish; do not interpret this as immediate
inference interruption. Each model may retain one resident process
under the existing lease policy. Kokoro/Nano GPU memory depends on
the total number of loaded models;
Turbo uses CPU inference. OBS splits Turbo input into at most 128-byte
chunks to bound inference duration; the worker rejects longer direct
requests. Long text playback may have synthesis gaps. No real-time
throughput claim is made.

## Pinned runtime and model

Use audio.cpp commit
`db1869d0d53c7839b79cf7c1a16d36cc20102307` and its vendored GGML
with a CPU-only C ABI build:

```sh
cmake -S . -B build-turbo -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=chatterbox_turbo \
  -DAUDIOCPP_BUILD_C_API=ON -DENGINE_ENABLE_CUDA=OFF \
  -DENGINE_ENABLE_VULKAN=OFF
cmake --build build-turbo --target audiocpp -j4
```

Retain the library license and header. The local CPU-only library is
5.9 MiB and has no CUDA/NCCL shared-library dependency. Do not configure
the previously interrupted CUDA builds. Use the self-contained
[Turbo Q8 GGUF](https://huggingface.co/audio-cpp/audio.cpp-gguf/tree/047ec541cc3f7e50c583acd56e0177ae286c4078/Chatterbox-Turbo-GGUF)
at revision 047ec541cc3f7e50c583acd56e0177ae286c4078, filename
`chatterbox-turbo-q8_0.gguf`, 699102368 bytes, SHA256
`acf02bed2b9ac94e271a01f556526818161a28009120f2337b9e8e10035c42b6`.
The upstream conversion is documented as MIT and is derived from
Resemble AI's Turbo via the cstr GGUF repack. No model weights or
runtime binaries are committed.

Append both options to the existing reader command, preserving any
Kokoro-native options:
```sh
--native-turbo-library /path/to/libaudiocpp.so.0.1.0 \
--native-turbo-model /path/to/chatterbox-turbo-q8_0.gguf
```

## Qualification and comparison

The earlier upstream CUDA v0.9.6 server was qualified on RTX 2070
SUPER (private loopback listener used only during measurement).
The same sentence as [Kokoro qualification](native-kokoro-qualification.md)
was synthesized four times with the built-in voice. The restored
Nano baseline used the actual existing Torch environment and
verified model assets at revision
`71ccd1d0081b430592cea481f4307e764e07bc64`.

| Path | Cold ms | Warm median ms | Output seconds (last) | Warm GPU allocation |
| --- | ---: | ---: | ---: | ---: |
| Turbo Q8 upstream CUDA server | 4331.7 | 1245.2 | 3.56 | 1664 MiB |
| Nano Torch CUDA | 27158.9 | 1092.2 | 3.52 | 2072 MiB |

Turbo server timing includes localhost HTTP. Nano timing includes
Engine.synthesize and cold import/model load from external disk.
The different model families, runs, output durations, and load paths
mean this is not a controlled kernel benchmark. Turbo was not faster
on warm requests in this sample. It used less GPU memory and a
smaller model file. An expressive-tag request returned 4.44 seconds
of valid audio; subjective quality is not established.

The existing Nano snapshot had four required broken content-addressed
links. All required files were restored from the pinned revision to
the external Audio library; four missing blob links were redirected
to exact verified content, without overwriting valid cached data or
restarting the service. The old link targets are retained in the local
qualification receipt. The unused non-meanflow checkpoint is not
required and was not restored. Both the built-in voice and a synthetic
7-second reference request passed real synthesis; a subsequent
built-in request also passed. The actual installed Nano socket
returned valid WAV for normal and reference-conditioned requests.
This tests reference conditioning, not objective speaker similarity.

## CUDA quarantine

Two locally built CUDA C ABI attempts stopped without returning the
first speech result, including a rebuilt library after a machine
restart. No persistent previous-boot journal was available, so the
precise cause and any causal relationship to the restart are unproven.
The same rebuilt library synthesized on CPU with CUDA hidden (10.795 s
cold, 3.942 s warm, 2.16 s of audio), establishing that the model and
C ABI are functional. The pinned Turbo T3 decode and masked S3Gen
attention graphs use ggml_flash_attn_ext, but both local CUDA builds
set GGML_CUDA_FA=OFF. GGML then reports no supported generic CUDA
FlashAttention kernel. Upstream defaults it ON. This is a concrete
configuration discrepancy, not a proven root cause. A new CUDA
candidate must retain FlashAttention and be tested in an idle
environment with instrumented stage/driver telemetry before any
installation. No further Turbo CUDA run was attempted while OBS
was open. The earlier upstream CUDA server benchmark remains a
separate, successful measurement and does not qualify the C ABI
build. CPU-only native Turbo is a deliberately separate, slower
option rather than a silent CUDA fallback.

## CPU-only acceptance (2026-11-25)

Pinned 5.9 MiB library SHA256
`a0392fface60621ed63df109a4e3fa307d0d4c7eff673834c86d48d6630791f0`.
Actual CPU-only worker produced 2.16 seconds of speech in 11.01 s
cold (including model hash verification), then 4.68 seconds of tagged
speech in 6.79 s warm. The process was killed, explicitly failed and
recovered on a new request. Lease eviction passed. The staged six-effect
OBS module passed built-in-voice selection, sampling controls, actual
clocked playback, cancel/recovery (3 completed, 0 failed, 0 invalid
samples), native CUDA Kokoro, ONNX Kokoro and Piper regression. The
complete focused test runner exited 0, including Unix-socket lease
eviction and explicit rejection of reference audio.
The CPU-only variant is opt-in and has no native GPU dependency.
Long multi-chunk completion, subjective audio quality, and the full
expanded Sound suite have not been qualified here.

## Tests

`python3 tests/verify-native-turbo.py` checks input policy. Pass
library and model paths for real CPU requests, sampling controls,
reuse, child failure/recovery and eviction.
`tests/verify-native-turbo-obs.sh` additionally exercises the real
OBS source, cancellation/recovery, leases, unsupported reference
options, and existing native Kokoro/ONNX/Piper regression paths
with an isolated reader. Set SOUND_TURBO_LIBRARY, SOUND_TURBO_MODEL,
SOUND_KOKORO_LIBRARY, SOUND_KOKORO_MODEL, SOUND_READER_PYTHON,
SOUND_READER_ASSETS and SOUND_STAGE.
