# Native RVC CUDA qualification (2026-11-25)

RVC is still quarantined in Sound. This checkpoint qualifies the native
GGML route and the proposed overlapping-window geometry; it is not an
installed OBS filter. The existing ONNX filter remains unchanged.

## Pinned inputs

- audio.cpp local source reference:
  https://github.com/0xShug0/audio.cpp/tree/db1869d0d53c7839b79cf7c1a16d36cc20102307
  (native RVC model spec, HuBERT, RMVPE, synthesizer, optional retrieval).
- Tested executable: upstream v0.9.6
  audio-v0.9.6-bin-ubuntu-x64-cuda12.8-colab.tar.gz, SHA256
  0cd6629b4e328a21bcf4a2359fe7ed615e1dc562073d01c8723c45cb7b724ffe.
  audiocpp_server SHA256
  03194177a8a7814add8ca011adb3cca94f8ed65644d046160780d8473aea3b05.
  This is the existing release artifact, not a new custom build.
  It requires NCCL; the test used the existing Chatterbox environment's
  library without changing system dependencies.
- GGUF package:
  https://huggingface.co/audio-cpp/audio.cpp-gguf/tree/46877f63ff8a2e4704b4925176a40e9b3e9d456e/RVC-GGUF
  rvc-f16.gguf, 1260187648 bytes, SHA256
  09bd8f233e7ae82697826513c94d754c3904e58e30ad503b83437defccf99a72.
  Stored on the external AI library; no weights in git. The collection
  labels the package MIT, but individual embedded voice/checkpoint
  provenance and redistribution permissions remain unverified. Use only
  voices that the operator is entitled to use. No weights are redistributed.
- Input: synthetic Kokoro speech from the earlier qualification, resampled
  to 16 kHz mono PCM16. No user microphone or real-person reference recording.

The release's documented RVC session default, F32 runtime weight storage,
works with the F16 package. The first CPU test explicitly selecting F16
runtime storage aborted in GGML's binary operation (dst F32, src0 F32,
src1 F16). Do not enable F16 or Q8 runtime storage merely because the
option is advertised. CPU F32 is a diagnostic, not an automatic fallback
for an explicit CUDA request. Other runtime precisions are unqualified.

## Measurements

RTX 2070 SUPER, 7797 MiB reported usable VRAM. Same synthetic fixture,
built-in default voice, retrieval disabled, requested output mono 48 kHz,
F32 runtime weights, 6 CPU threads. CPU devices hidden in the CPU test.
Time includes local HTTP transport and WAV/base64 conversion.

| Input | CPU ms | CUDA ms | Output |
| --- | ---: | ---: | ---: |
| 1 s cold | 5617.5 | 4174.5 | 0.98 s |
| 0.5 s warm | 1494.8 | 276.7 | 0.48 s |
| 1 s warm | 1831.6 | 313.4 | 0.98 s |
| 2 s warm | 2562.7 | 392.5 | 1.98 s |
| 1 s, +4 semitones | 1905.1 | 322.3 | 0.98 s |

CUDA process allocation at the end of the run was 1290 MiB. OBS was
open; the test started with 4054 MiB free and used the server's 1536 MiB
memory reserve setting. This is a bounded coexistence observation,
not a guarantee against GPU contention. CPU was slower than real time.
Both backends returned valid, nonempty audio; the +4 semitone request
returned different output. Subjective voice quality and pitch accuracy
were not independently evaluated.

## Paced overlapping-window test

The actual existing C++ Sound SolaStitcher receives real native converted
PCM from the same resident release server. Each one-second input window
covers t-250ms to t+750ms, advanced by 500ms. At 48 kHz, select a region
starting at output sample 11520 (240ms) with 24000 hop samples,
1920 overlap samples and 960 search samples. This leaves room for the
observed 20ms-short output. SOLA selects an offset within the search
region, emits exactly one hop, and retains the unplayed overlap.
First window has zero left context; no real audio device was captured.

24 paced hops (12 seconds), total 576000 frames:
median 328.1 ms, max 352.8 ms including conversion, PCM transfer and
SOLA; zero 500ms deadline misses. Maximum absolute inter-hop sample
step 0.04086. WAV retained for audition. No claim of speech quality or
alignment against a real OBS audio clock. This geometry implies at least
750ms lookahead from the emitted hop start plus inference and scheduling;
an initial conservative fixed delay around 1.25–1.5 seconds is a
candidate to qualify, not a measured end-to-end latency.

## Reproduce

Use an isolated local test only; this server has generic local task
routes and should not be exposed to other hosts. Verify the release
archive first. Run with a synthetic or authorized mono 16 kHz PCM16
speech WAV (at least two seconds), required local CUDA dependencies,
Python 3.12 and a C++17 compiler. No downloads occur in the test.

```sh
c++ -std=c++17 -O2 -I src tests/verify-rvc-native-stitch.cpp -o /tmp/rvc-stitch
python3 tests/verify-rvc-native.py /path/to/audiocpp_server \
  /path/to/rvc-f16.gguf /path/to/synthetic-speech.wav \
  /tmp/rvc-stitch /path/to/test-output --hops 24
```

Use --backend cpu --hops 0 to reproduce the CPU baseline. The test
checks hashes, geometry, finite output, resident paced deadlines and
child cleanup. It refuses CUDA testing below 4000 MiB free at startup;
the server guard is not a complete reservation of GPU memory.
The output directory should be a new local qualification directory.

## Remaining integration

Replace the quarantined ONNX adapter only after the native private
worker is qualified. Use a pinned narrow audio.cpp C ABI build with
CUDA FlashAttention retained, rather than repurposing a build that
previously failed with FlashAttention disabled. Verify real short
windows, session retention, failure/cancellation/recovery and model
compatibility; no implicit CPU fallback. Avoid exposing a generic
HTTP listener in production. Add bounded RT-safe stereo capture,
16 kHz resampling, timestamp/epoch handling, exact-hop SOLA,
aligned delayed dry on failures, model/voice/pitch/retrieval controls,
and explicit opt-in registration. Test actual libobs at both rates,
long-run drift, contention and safe unload before installation.
No model/license claims beyond the observed package metadata.
