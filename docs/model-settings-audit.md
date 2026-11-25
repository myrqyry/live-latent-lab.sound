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
| Whisper | Local model, device, language, translation, prompt, threads, phrase and caption timing | Greedy strategy is fixed. Temperature, greedy best-of, beam strategy/size, no-speech/logprob/entropy thresholds need an advanced decoder settings pass and actual caption regression. |
| Parakeet | Model/runtime, device, phrase and caption timing | Auto language; wrapper does not expose language override, vocabulary prompt, decoder strategy or translation. Do not add ineffective fields. |
| Nemotron | GGUF/runtime, device, language locale, threads, right context, segmentation | Current private ABI exposes only these model/session options. Other decoder controls need extension and qualification of runtime, not speculative UI. |
| Zipformer | Model directory, device (CPU-qualified), threads, segmentation | Greedy decode fixed; modified beam search, active paths and hotword tuning need actual sherpa configuration and streaming latency qualification. |
| YAMNet | Model/runtime, compute delegate | Full category vector deliberately preserved for inter-plugin consumers. Threshold/top-k/allowlist are output-selection controls; adding per-view filtering must not truncate the shared 521-category signal. |
| HTDemucs | Stem selection, worker socket; worker model directory and precision CLI | Shared worker fixes model htdemucs, shifts=1, overlap=.25, default segment length, segments enabled. Device/model/quality controls should be shared-worker settings, not independent misleading per-filter knobs. Measure deadlines for each supported configuration. |

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
model controls. Next cover Whisper/Zipformer decoding, HTDemucs
shared worker configuration, remaining native speech conditioning,
and safe advanced SA3 output/continuation controls. Treat settings
that require a different model, new ABI, new transport or changed
live timing as explicit extensions; do not expose them as working
until tested. Revisit UI visibility and grouping so settings are shown
only for the relevant backend, and validate backend-specific choices.
