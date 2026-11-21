# RVC repair checkpoint: model contract and safe bypass

Status: **partially repaired, still quarantined** (2026-10-13).

The `rvc` experiment in Live Latent Lab: Sound is **not** verified voice
conversion. The default plugin still builds with **zero** effect registrations.

## Reproduce this boundary

```sh
./tests/verify-rvc-safety.sh
./tests/verify-quarantine.sh
```

The RVC safety suite builds a **disposable** RVC-only OBS module, bundles
ONNX Runtime and checks that exactly one effect is registered; it then runs:
- A genuine ONNX Runtime model-loading test against a corrupt file, which
  must leave `is_ready=false`.
- A genuine, well-formed but **non-RVC** ONNX sigmoid graph to verify
  that schema inspection rejects incompatible model metadata. This tests
  rejection, not positive inference.
- Direct checks of frame/channel tensor dimensions and missing-model
  behavior.
- A real libobs stereo generator passed through the production RVC
  filter, captured *after* the filter callback. With no model or invalid
  model path, both channels remain dry with unchanged RMS and balance.
- An explicit exported RVC generator sample-rate selector. Unknown
  rates or missing model paths do not enable the filter.

The ONNX sigmoid fixture in `tests/fixtures/onnxruntime_non_rvc_sigmoid.onnx`
comes from ONNX Runtime's small dataset samples (Microsoft, MIT license).
It is a deliberately incompatible real graph, **not a mocked RVC inference**.

## Corrected assumptions

- RVC generator export expects phone features shaped `[1, frames, channels]`,
  NOT `[1, channels, frames]`; standard v1/v2 channel counts are 256/768.
- `pitch` is a coarse INT64 melody encoding in the exported graph;
  `pitchf` is continuous F0 in Hz. Feeding both as float and using
  semitone offsets for `pitchf` was wrong.
- The HuBERT padding mask must be BOOL in the supported model contract.
- HuBERT embeddings typically use 50 Hz; the generator consumes roughly
  100 Hz, so features require temporal upsampling.
- The synthesized output rate depends on the voice model. Guessing 32 kHz
  for every export is not safe. The test implementation requires explicit
  32, 40 or 48 kHz selection.
- A voice ONNX session alone cannot establish readiness: voice, HuBERT
  and F0 models must *all* load and match their declared input/output
  names, dtypes and supported shapes. Failed inference demotes readiness.
- The existing RVC path remains mono-only: **stereo passes through dry**
  rather than copying converted left audio onto the right channel.
  Missing output frames are also dry, not zero-filled.
- No huge ring buffers or idle worker are created with missing models.
- Metadata ownership is verified: the guard previously read shape info
  after its owning ORT TypeInfo had been destroyed. A real wrong-model
  test caught this via a crash; the ownership bug is fixed and retested.

Relevant upstream shape semantics:
- RVC-Project/Retrieval-based-Voice-Conversion `rvc/modules/onnx/export.py`
- RVC-Project/Retrieval-based-Voice-Conversion-WebUI `infer/vc/pipeline.py`
- RVC v2 ONNX inference wrappers for HuBERT doubling and F0 quantization

## Existing native alternative: audio.cpp (investigation, 2026-10-13)

**This changes implementation strategy, not verification status.**
The upstream [audio.cpp project](https://github.com/0xShug0/audio.cpp)
is a ggml-powered native audio runtime with an existing **RVC** implementation.
The published [RVC guide](https://github.com/0xShug0/audio.cpp/blob/main/docs/audio_tools.md#rvc)
documents a self-contained F16 GGUF package with HuBERT/RMVPE and packaged
voices, and accepts a user `.pth`/`.pt` voice checkpoint and optional FAISS
retrieval index. Its [GGUF collection](https://huggingface.co/audio-cpp/audio.cpp-gguf)
lists an `RVC-GGUF` package.

Crucial distinction: **audio.cpp RVC is documented as OFFLINE WAV-to-WAV
conversion, not streaming.** The separate `meanvc2` voice-conversion family
offers a streaming mode (120 ms / 40 ms). Do not feed successive tiny WAVs
to an offline RVC CLI and call it an OBS-ready real-time voice filter.

Next verification path:
1. Build audio.cpp separately at a pinned revision; do not touch the normal
   Sound build or currently installed OBS plugin.
2. Obtain the RVC GGUF as a separately controlled asset and test a **real**
   offline conversion: sample rate, waveform, channel count, duration, audible
   identity transformation, CPU/GPU speed and peak memory.
3. For **live OBS**, independently qualify MeanVC2 streaming or verify a
   genuine stateful low-latency RVC route. Measure latency, overrun, buffering,
   drift, channel mixing, dry fallback, reload and teardown.
4. Only after real audio evidence, consider a thin backend adapter behind the
   existing experimental Sound registration gate.

At this *initial* checkpoint, no `audiocpp_cli`, `audiocpp_server`, or
RVC GGUF was found locally. No audio.cpp model inference had been tested.
**The missing three ONNX models block the handwritten adapter, NOT
audio.cpp's packaged GGUF path.**

**Follow-up later on October 13:** audio.cpp was successfully built on
Mint with CPU-only RVC + MeanVC2, including its streaming C ABI. A 342 MB
MeanVC2 Q4 model then produced real converted audio through both
the CLI and independent C ABI probe. The model hash, streaming chunk
timing, approximately 9.1 GiB peak RSS, saved WAV audition and limitations
were recorded in a separate evaluation note **not included in this curated
release repository**. RVC GGUF inference remains **untested**, and the
Sound RVC effect stays quarantined.

## Native RVC user-checkpoint follow-up (2026-10-14)

A real external audio.cpp native RVC conversion has since been
verified independently using an IBM 7094 v2 `.pth` and optional FAISS
`.index`, producing working 40-kHz WAVs. The original audition evidence
is **not included in this curated release repository**.
This does **not** validate the handwritten ONNX filter, which remains
quarantined, nor does it prove live RVC inference. The verified
MeanVC2 bridge additionally accepts WAV audio generated by this RVC
checkpoint as a target-reference experiment; it uses a different model.

## Still blocking graduation

- **No compatible complete ONNX model trio located** in local project,
  cache, Music, or common models directories. Some HuBERT/RMVPE PyTorch
  .pt assets exist, but they are not compatible ONNX files.
- No successful actual HuBERT/RMVPE/generator inference, converted
  waveforms, listening test, or generator sample-rate confirmation.
- No measured latency, alignment, chunk boundary smoothing/crossfades,
  pitch continuity, resampling quality, and mono/stereo routing strategy.
- Model reloading is synchronous in OBS settings; live swaps and
  worker/queue reset on model changes need more careful lifecycle work.
- Model-export variants outside the exact checked schema are rejected,
  not silently misinterpreted.
- These tests cover the isolated source output, not final OBS
  mixing/encoding or a real microphone on a production OBS scene.

**The RVC effect remains disabled in the normal build.** Nothing
installed or pushed; no real model has been represented as working.
