# Live Latent Lab: Sound — Hugging Face stem candidates and no-fake-stems checkpoint

Date: 2026-11-12. This is an evidence-led integration note, not a claim that live separation works.

## Sources confirmed on Hugging Face

1. StemSplitio/htdemucs-onnx — https://huggingface.co/StemSplitio/htdemucs-onnx . Standard four-stem ONNX export, MIT-marked. Model card gives fp32 316 MB and fp16-weight 166 MB, but similar RAM/runtime costs. Input tensor name `mix`, shape [1,2,343980], 44.1 kHz stereo (7.8s fixed); output `stems`, shape [1,4,2,343980] ordered drums/bass/other/vocals. Overlap-add is required for longer material; published timing benchmarks are not measurements on the user's RTX 2070 SUPER.
2. notabilia/uvr5-models — https://huggingface.co/notabilia/uvr5-models ; UVR-MDX-NET-Inst_HQ_3.onnx is 66.8 MB (two-stem separation family). This is not an HTDemucs tensor-compatible replacement. Exact model input/output/STFT preprocessing must be inspected with actual ONNX metadata and reference code before any inference is wired.

## What is wrong with the current OBS source integration

- `src/engines/demucs_engine.hpp` assumes tensor names `audio` and `sources`, and variable `[1,2,frames]` input. The HTDemucs model card explicitly gives `mix`/`stems` and fixed 343980 samples. Its input contract cannot be satisfied merely by selecting an ONNX file in the existing UI.
- `src/demucs-source.cpp` captures only the first audio channel; the Demucs engine mirrors it to stereo, then averages predicted stereo back to mono. This destroys channel independence and is not acceptable for production stem separation.
- The source currently collects at least two seconds of audio, while the referenced HTDemucs export needs 7.8 seconds. True delayed-live stem playback needs deterministic timestamps, overlap-add, per-stem synchronized rings and measured buffering; a quick offline audio file test is not fulfillment of the live-stream request.
- Prior fallback behavior duplicated input music into all four stems when inference was missing/failed. This checkpoint **removed those fake separated outputs** in both engines, the Demucs source and the multichannel Demucs filter. The custom-mix/DMCA-safe filter modes now output no new audio without a ready model; ordinary single-stem filter dry bypass remains separate. The standard engine rejects model outputs with fewer than four stems instead of filling missing tracks with invented silence. The new `stem_output_guard.hpp` rejects missing, incomplete, incorrectly sized or non-finite batches before writing any new stem. Pure C++ tests and source-pattern checks pass with `tests/verify-demucs-no-fake-stems.sh`.

## Verified and unverified

- `tests/verify-demucs-no-fake-stems.sh` passed, including no-callback-on-invalid-batch checks.
- The actual `src/demucs-source.cpp` passed C++ syntax-only compilation against local libobs 32.0.1 and existing ONNX Runtime headers. The multichannel filter's source changes were not linked into a running OBS module. This is **not** live OBS audio capture with a Demucs model.
- Existing mic-only OBS DSP and default zero-registration quarantine must remain passing.
- Advanced `DemucsCppEngine` syntax validation in an ad-hoc build was blocked by the old demucs.onnx subproject expecting an internal ONNX Runtime header that is not present at the cached include path. No claim is made that the advanced engine builds on the current dependency graph.
- **No HF model downloaded, no GPU inference tested, no OBS plugin replaced, no running OBS session or profile changed.** The system drive is nearly full; never use a guessed download/model installation path.

## Next implementation boundary

Use a fixed-version model and its reference preprocessing with a local-only model process, then prove actual four different, synchronized 44.1/48 kHz stems in a disposable libobs runtime using real audio and missing-model/late-model/restart tests. Run separate latency and sustained load measurements on the RTX 2070 SUPER. Do not promote `demucs_source`/`demucs_filter` out of quarantine until those tests establish a practical delayed-live implementation. Never present dry input copies or unverified silence as successfully separated stems.