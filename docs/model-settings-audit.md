# Model settings coverage

Current scope: the seven-effect Sound candidate on 2026-11-25.
This records the actual controls and the remaining gaps, not a promise
that every upstream runtime option is safe for the live path.
An editable setting should be wired to the selected model, have bounded
input validation, and be covered by real runtime acceptance. Keep
internal allocation and transport limits distinct from inference knobs.
Do not add controls for unsupported model families or silently ignore
a requested option. Preserve old defaults and explicit device policy.

| Effect / backend | Exposed controls | Remaining or intentionally fixed |
| --- | --- | --- |
| RVC native CUDA | Packaged voice, checkpoint, speaker ID, retrieval index and blend, semitone shift, median filtering, RMS envelope mix, unvoiced protection, wet/dry | RMVPE only; host output rate; F32 weights, 1-second model padding, single resident cache slot and window/SOLA geometry fixed. Offline F0 CSV and long-audio split settings are not applicable as-is to live windows. Nonzero speaker IDs need model-specific qualification. |
| Native Kokoro | English voice, speed, seed, model text chunk size | Voice-matched language; explicit phoneme-list input and blend syntax not yet wired; timestamp output needs a consumer. Non-English packs not qualified. Arena sizes/precision remain pinned. |
| Kokoro ONNX | Voice, speed, language, phoneme input, trim, sentence/clause pause | Single qualified ONNX model; device policy service-level. No invented seed control. |
| Piper ONNX | Voice, phoneme length, noise, phoneme width noise, normalize, volume | One qualified single-speaker model; multi-speaker selector and local voice catalog need qualification. Device policy service-level. |
| Chatterbox Nano | Temperature, top-k, top-p, repetition penalty, reference clip, reference loudness normalization | Existing Torch service's generation implementation is the authority; no native-Turbo-only options copied across. |
| Chatterbox Turbo CPU | Temperature, top-k, top-p, repetition penalty, seed, token cap, model text chunk size | Built-in voice only; 128-byte transport cap, CPU-only qualified. No silent CUDA substitution. |
| Edge / TikTok | Edge voice/rate/pitch/volume; TikTok voice | Existing explicit cloud-only APIs, not local neural models. |
| SA3 Music/SFX | Prompt/negative prompt, duration, steps, seed, CFG, distribution schedule, generation/continuation padding, transform strength, LoRA controls, contextual input and automation, device | Custom distribution coefficients, latent rescale/shift, native normalization/limiter and continuation parameters not yet exposed; must account for bounded output, fixed continuation seam and reserve policy. SAT-only sampler/sigma controls do not apply to current SA3 variants. Advanced properties are not all mirrored in dock. |
| SAME-S | Morph A/B, blend/ramp, freeze position, smear radius, grain length, effect amount, live reserve | Fixed codec/model, latent geometry and frame bounds; no additional learned model controls in the current codec ABI. |
| DDSP | Eleven timbres, A/B morph, semitone pitch, harmonic/noise gain, expression, gate, wet gain/mix | Feature/decoder recurrent tensor contract, CPU runtime and resampling geometry fixed. All current creative controls available; variable latency and confidence gating would be new pipeline features. |
| Whisper | Local model, device, language, translation, prompt, threads, phrase and caption timing, greedy/beam strategy, best-of, beam size, initial/fallback temperature, no-speech/logprob/entropy thresholds and suppression toggles | Advanced settings apply on next phrase; ranges bounded, full extrema unqualified. Timestamps, context reuse and output formatting remain fixed for phrase-based captions. |
| Parakeet | Model/runtime, device, phrase and caption timing | Auto language; wrapper does not expose language override, vocabulary prompt, decoder strategy or translation. Do not add ineffective fields. |
| Nemotron | GGUF/runtime, device, language locale, threads, right context, segmentation | Current private ABI exposes only these model/session options. Other decoder controls need extension and qualification of runtime, not speculative UI. |
| Zipformer | Model directory, device (CPU-qualified), threads, segmentation, greedy/modified beam search, active paths and blank penalty | Decoder changes reload recognizer. Ordinary-text hotword biasing needs the exact matching BPE vocabulary; pinned model package contains tokens.txt but no bpe.vocab. |
| YAMNet | Model/runtime, compute delegate | Full category vector deliberately preserved for inter-plugin consumers. Threshold/top-k/allowlist are output-selection controls; adding per-view filtering must not truncate the shared 521-category signal. |
| HTDemucs | Single stem, instrumental, custom mix with four levels/mute/solo, separate outputs (silent parent), optional 2 s cross-window blending and worker socket; normal Stem Output source binds by filter UUID and stem; shared worker CLI: model directory, precision, device, shifts, overlap, segment length | Only HTDemucs four-stem model, synchronized stereo mix, split enabled, 8-second windows and 12-second delay. Worker-level graphical settings not implemented. CPU and all combinations/extremes are not live-qualified; missed deadlines mute instead of silently falling back. |
| Mel-Band RoFormer | Vocals/instrumental single or custom levels/mute/solo, separate output mode, v2 worker socket and retry | Pinned local model, CUDA FP32, 2.8 s context, 700 ms hop, 350 ms future, 140 ms crossfade, 2.1 s fixed prefill. CPU/other model or precision not qualified; no fallback. |

## Acceptance in this pass

RVC: native request contract and real OBS processing with a custom
checkpoint/index at both rates; packaged voices and embedded indexes
tested individually. Nondefault RMS/protection/median options and
invalid option rejection. See native-rvc.md.

SA3: actual Small Music generation with nondefault guidance, schedule
and tail padding, replay/cancellation/missing/recovery/disable at both
rates. Initial lifecycle test failed its completion deadline; isolated
single generation and full retry passed. No blanket qualification of
all parameter extremes or continuous/SFX configurations.

Native speech: reader option validation, retention/device-policy
unit tests, real native Kokoro CUDA and Turbo CPU requests with
nondefault model settings. Both native engines passed real OBS playback through an isolated
reader service, including nondefault controls, cancellation and
recovery (three completed requests and zero failures per engine).

## Remaining accepted scope

The settings audit does not close the request for full supported
model controls. Whisper/Zipformer decoding is now qualified at both host rates. Next cover Zipformer hotwords once a matching BPE vocabulary is available, graphical shared HTDemucs worker management, remaining native speech conditioning,
and safe advanced SA3 output/continuation controls. Treat settings
that require a different model, new ABI, new transport or changed
live timing as explicit extensions; do not expose them as working
until tested. Revisit UI visibility and grouping so settings are shown
only for the relevant backend, and validate backend-specific choices.


## Speech recognition decoder acceptance (2026-11-25)

Installed Whisper default-param ABI was queried rather than assuming
upstream defaults (temperature 0, fallback .2, best-of and beam 5,
entropy 2.4, logprob -1, no-speech .6, blank suppression on, non-speech
suppression off). The advanced Whisper settings are request-level,
whereas Zipformer decoding/beam paths/blank penalty are session-level.
Config policy and actual backend-specific property visibility passed.
Real OBS JFK fixture at both rates, using Whisper CPU beam size 3,
no-speech .55, temperature fallback disabled and subsequent live
temperature .1, passed expected words, routing, hot stability, dry
audio, stopped-source flush, mute, off/missing/recovery. Final inference
2063/2279 ms (48/44.1 kHz). Zipformer CPU modified beam with 4 active
paths and blank penalty .1 passed both rates, 30 distinct partials,
4 finalized sequences, inference 6/7 ms. First words still imperfect.
This does not qualify every decoder extreme, CUDA Whisper, or improve
WER conclusively. Hotwords are not yet exposed: the exact pinned
model revision does not include a BPE vocabulary, which the Sherpa
ordinary-text contextual-biasing API requires for this model family.


## HTDemucs shared quality settings (2026-11-25)

The worker now accepts --device auto/cuda/cpu, --shifts 0–2,
--overlap .05–.5 and --segment-seconds 1–7 (omit for model default).
Existing precision and model-directory options remain. Explicit
device requests are checked, and autocast with CPU is rejected.
The fixed geometry and original quality defaults are unchanged
(1 shift, .25 overlap, default segment, split enabled, FP32).
An initial real CUDA test using shifts 0, overlap .1, segment 4
missed the 4-second budget on its first cold request (4.807 s),
correctly yielding silence. A synthetic 8-second stereo separation
now runs before the private socket is made ready. The warmup uses
the exact separation path, validates all four stems and discards
its ephemeral output. The installed adapter still reloads weights
per request; this is runtime/cache warmup, not model residency.
Actual 48 kHz OBS retry with those nondefault settings passed:
warmup 2.240 s; two real windows 1.309/1.208 s; 24 seconds of
input, correct first-11-second silence and distinct stereo vocal
output. Eight worker unit tests passed. No real-time guarantee at
higher quality, under contention, with CPU, or for all allowed
values. See expanded-build.md for worker CLI/verification.

The existing default quality regression also passed the isolated
pipeline, socket contract, eight worker tests, registry, missing
worker silence and real GPU OBS audio. Defaults: warmup 2.300 s;
two windows 1.366/1.241 s. No live installation or service changes.
