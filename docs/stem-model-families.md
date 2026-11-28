# Separation backend expansion — 2026-11-27

The current Sound stem transport has a qualified four-stem HTDemucs
implementation. It must not be treated as the only separation model
family. UVR is an application/catalog, not one interchangeable tensor
contract. The existing audio-separator 0.47.0 environment already has
MDX (ONNX), VR, Demucs and MDXC/RoFormer implementations. Its local
supplementary models.json includes 84 RoFormer entries; catalog
presence is not proof that weights are cached or live inference works.

## Local evidence and priorities

| Candidate | Actual outputs | Current evidence |
| --- | --- | --- |
| HTDemucs resident | drums, bass, other, vocals | Real OBS GPU acceptance, existing v1 |
| Mel-Band RoFormer | vocals and residual instrumental | Exact cached Kijai asset, direct CUDA probe below |
| UVR-MDX-NET-Inst_HQ_3.onnx | instrumental and vocals | Existing Stem Splitter catalog default; not cached in its configured model dir |
| BS-RoFormer | checkpoint-dependent; two or more stems | Native audio.cpp source support; not locally qualified |
| VR Architecture | checkpoint-dependent (karaoke etc.) | Installed adapter; not locally qualified |
| HTDemucs 6-stem | drums, bass, other, vocals, guitar, piano | Native audio.cpp source support; weights not identified in configured cache |
| SCNet / Band-SCNet / Spleeter | checkpoint-dependent | Research candidates; no Sound runtime qualification |

Native path: local audio.cpp checkout 26b8927 exposes named-audio result
accessors, sep task, htdemucs, htdemucs_6stems, bs_roformer and
mel_band_roformer families in source/model specs. These modes are
currently described as offline. The installed RVC/VAD library's
actual registry contains only marblenet_vad, rvc and silero_vad;
it must not be replaced or described as a ready separation runtime.
Build or stage an isolated native separation runtime and verify
registry, explicit CUDA, weight compatibility and named stereo
outputs before integration. GGUF Q8/F16 are candidates, not
automatically faster or equivalent. Existing cached HTDemucs weights
can also be evaluated via the native converter. No new model
download or dependency installation occurred in this pass.

## Cached Mel-Band RoFormer direct probe

Kijai FP16 SafeTensors asset (456,479,072 bytes, 684 tensors):
SHA256 6119aef379a6c7264e0b37db65ae1e6488b8ca4a00baf56d6d244737b8488226.
Local hash exactly matches the published asset. Reused the file
in the existing ComfyUI model library without copying or modifying
it. Kijai's loader specifies the 384-dim, 6-layer stereo model with
60 bands and 441-sample STFT hop; the installed audio-separator
MelBandRoformer class strictly loaded all weights. Direct FP32
CUDA execution on RTX 2070 SUPER (torch 2.14.1+cu130), not native
FP16/autocast or the audio.cpp backend.

Probe input: local synthetic stereo speech/tone fixture, resampled
48 kHz to 44.1 kHz before timing; first three candidate windows
of 1.4, 2.8 and 8 seconds. Two separate executions, three runs per
geometry, first run excluded from warm median. Vocal tensor shape
equals input, finite nonzero stereo output, instrumental calculated
as mixture minus vocals, finite nonzero residual. This is not
independent learned instrumental estimation or a music SDR test.
Adapter default zero-DC policy, match_input_audio_length enabled;
no numerical parity claim against original Kijai inference.

| Context | First warm median | Repeat warm median | Peak allocated CUDA tensor MiB |
| --- | ---: | ---: | ---: |
| 1.4 s | 0.1532 s | 0.1520 s | 1030.2 |
| 2.8 s | 0.2939 s | 0.2948 s | 1175.0 |
| 8 s | 0.9255 s | 0.9361 s | 1705.4 |

Peak is PyTorch allocated tensor memory, not total VRAM. Timings
exclude resampling, transport, playback, cross-window blending and
model load (1.34–1.39 s once per process, excluding SHA verification).
The first 1.4 s inference was 0.316–0.447 s. These measurements are
enough to justify a 1.4–2.8 s context experiment, not an end-to-end
latency guarantee. The second execution was the checked-in probe:

```sh
python tests/verify-stem-melband-model.py \
  --model /path/to/MelBandRoformer_fp16.safetensors \
  --fixture /path/to/stereo-48k-f32.raw
```

The current Stem Splitter venv has no safetensors package. For the
probe only, explicitly supplied the existing trusted Python 3.11
Chatterbox safetensors package directory with
`--safetensors-package /path/to/safetensors`. No global PYTHONPATH,
package installs or project venv mutations. A normal deployed backend
should declare and pin its own dependency instead.

## Architectural boundary

Preserve LLSTEM01 as four real named stems, fixed 8-second stereo
geometry and existing delay. Do not fill missing bass/drums/other with
zeros or mislabel an instrumental residual as Other. A two-stem or
six-stem implementation needs a separate versioned interface
with explicit model identity, supported stem names/geometry, rate,
latency and availability. Use named streams and one shared cursor
per producer; discover only genuinely available outputs, preserve
saved UUID references, invalidate old epochs on model/layout changes.
Distinguish model-internal overlap from cross-window stream blending.
Reuse current OBS mixer, dispatch queue, recovery and audio-source
routing rather than separate duplicate plugins. Keep all inference
off the callback; GPU/CPU choice and precision are explicit.

Next experiments:
1. Measure short-context Mel-Band on a licensed real stereo fixture,
   compare with full-context reference and actual mixture reconstruction,
   then evaluate overlap/hop/lookahead with quality metrics and latency.
2. Qualify the native GGUF RoFormer/HTDemucs path using isolated
   audio.cpp runtime; compare both quality and wall/VRAM costs, do
   not silently swap engines or lower quality.
3. Evaluate MDX with pinned assets and actual ONNX CUDA execution;
   use a real model descriptor rather than an arbitrary ONNX picker.
4. Implement the versioned variable-stem contract only when the
   candidate geometry and named outputs have been qualified.

Prior art:
- https://github.com/nomadkaraoke/python-audio-separator
- https://github.com/kijai/ComfyUI-MelBandRoFormer/blob/main/nodes.py
- https://huggingface.co/Kijai/MelBandRoFormer_comfy/blob/main/MelBandRoformer_fp16.safetensors
- https://github.com/nnyj/python-audio-separator-live — MDX/RoFormer
  live-device prototype, reports ~1.7 s on its own hardware; inspect
  algorithm and licensing before reuse, not a Sound benchmark.
- https://arxiv.org/abs/2609.35397 — Sep 2026 study of output readout
  and future lookahead for streaming offline checkpoints. A shorter
  readout can reduce algorithmic delay without shrinking the context;
  quality and computational cost remain distinct. Test on target GPU.
- https://github.com/james34602/SpleeterRT — older monaural approach,
  not a drop-in stereo replacement (local archive also exists).

No OBS changes, runtime install, push, new stem source or model-family
registration in this checkpoint.
