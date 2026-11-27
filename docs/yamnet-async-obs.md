# Native YAMNet asynchronous OBS audio candidate

Evidence date: November 13, 2026. This is a tested opt-in candidate,
not an installed Sound plugin or user-facing captioner.

## What is implemented

An explicitly gated Sound build (SOUND_EXPERIMENTAL=ON,
SOUND_EFFECTS=yamnet) compiles src/yamnet-worker-filter.cpp and
registers the yamnet_classifier_source ID. The old synchronous
src/yamnet-source.cpp remains excluded. The default Sound build
still registers no effects.

The OBS audio callback only mixes planar audio to mono using a fixed
stack scratch buffer and copies it to Sound's existing preallocated
single-producer/single-consumer ring buffer. It returns the original
audio unchanged. No model inference, resampling, filesystem operations,
locks, allocations or sleeps occur in that callback.

A worker opens an explicitly supplied existing MediaPipe Tasks 1.2.3
native library and YAMNet model, then uses libobs's audio resampler
to convert 44.1/48 kHz to 16 kHz mono. It classifies one-second windows
and publishes a copy of the top category through the read-only OBS
source procedure sound_yamnet_read_v1. That procedure returns category,
score, count, dropped samples, inference timing, callback count,
status and availability.

The capture queue requests three seconds of samples; the power-of-two
ring allocation provides 262143 usable samples at either supported rate.
A queue overflow invalidates inference rather than splicing discontinuous
audio samples.
Missing or invalid model files produce a visible error status without
affecting the upstream audio. Labels expire after 2.5 seconds of no
new classification. Model/runtime/delegate changes now reload on the
worker without recreating the filter.

A native crash was caught and reproduced by gdb during OBS shutdown.
After closing a classifier, XNNPACK/native thread-local cleanup still
needed code from the MediaPipe library, while the previous dlclose
unmapped it. The native wrapper now uses RTLD_NODELETE on Linux to
keep the native code mapped until process exit. Classifier/result
objects still close on worker teardown, and real OBS tests now exit 0.

## Reproduce the real OBS path

Run ./tests/verify-public-linux.sh from the Sound repository.
Use SOUND_MEDIAPIPE_LIB and SOUND_YAMNET_MODEL to select existing
user-owned files if the defaults are unavailable. No models download
or network access is required.

The test performs a fresh opt-in build, checks its unique build nonce
and exact one-filter registration, then injects synthetic planar
stereo audio through an actual libobs source and this production filter.

Observed outputs on yuzu-mint:
- 48 kHz and 44.1 kHz source rates pass.
- Silence classified as Silence, 0.800781 score.
- 440 Hz sine classified as Sine wave, 0.996094 score.
- Zero dropped samples in these fixtures.
- Unmodified stereo: left RMS 0.212132, right RMS 0.106066.
- A nonexistent model is rejected; dry audio remains unchanged.
- Stale results become unavailable once audio stops.
- Worker/model/library cleanup exits without a segfault.
- Only the selected YAMNet source is registered in the candidate.

Existing regressions also passed: verify-quarantine.sh (zero
default effects), verify-stem-live.sh
(contract and mocked sidecar only), and
the historical direct MediaPipe inference test (superseded by the public-profile runner).

The architectural reference was XWHQSJ/captionflow, but the transport
reuses Sound's own existing ring buffer. No upstream source was copied.
norihiro/obs-async-audio-filter addresses hardware clock drift, not
asynchronous model inference.

## Category inventory and individual signals (2026-11-18)

The opt-in worker now requests all model categories (bounded to 1024);
the existing top-class procedure remains compatible. After the first
successful inference, `sound_yamnet_list_v1(in ptr inventory)` fills a
caller-owned OBS data object with version=1 and a categories array
(label and model index). This uses actual returned model labels, not a
hardcoded AudioSet catalog. Empty before first inference; names remain
discoverable if the result later expires within the same generation.
A reload or fault invalidates the old inventory until fresh inference.

`sound_yamnet_score_v1(in string category, out float score, out bool available)`
returns the exact category's latest finite score (including valid zero).
Unknown categories and stale/failed/overloaded results are unavailable.
The reader should also check the OBS producer is attached and enabled.
Neither call runs inference or touches the audio callback. The inventory
is control-thread metadata; read scores only outside the audio callback.
The 1-second classification cadence and 2.5-second expiry remain unchanged.
This is classification, not a precise sound onset detector.

## Remaining before real user acceptance

This has no GUI text display, captions or text-file writer yet.
The read-only procedure requires a consumer. Sustained callback deadlines, multiple concurrent filters, a real
microphone, and full OBS GUI sessions remain to be qualified.
Targeted overflow/recovery is now covered below.

The live multi-stem separation work remains a separate accepted goal;
YAMNet is not a substitute. Do not install or push this candidate as
if the complete Sound audio feature were ready. Keep existing scene
collections, plugins, model weights and installed Sound quarantined.


## Delegate selection (2026-11-24)

The filter now exposes **Compute delegate** with CPU (default), Auto
(GPU then CPU) and explicit experimental GPU (no fallback). Model,
runtime and delegate can reload live. CPU is the recommended mode
on the qualified Linux setup.
The read-only `sound_yamnet_device_v1` procedure reports requested,
selected and status separately from the existing result ABI.
A failed explicit GPU request publishes an error, not a CPU result.

The local MediaPipe Tasks 1.2.3 and valid YAMNet model were checked
directly and through actual OBS at both 44.1/48 kHz. CPU classification
took about 2–3 ms per one-second input. The available GPU delegate failed
graph initialization; Auto successfully selected CPU. No claim of GPU
acceleration is made for this environment. An incomplete cached model
was rejected by the native runtime; the qualified local 4,126,810-byte
model has SHA256
`4d8b4a53282dc83ef04e3e7dbc4fbc98082e34e44ed798e16c3a0cdd4c584faf`.
No model files or user scene settings are overwritten.

For reproducible current acceptance use `tests/verify-public-linux.sh`
with SOUND_MEDIAPIPE_LIB and SOUND_YAMNET_MODEL, or build the
`tests/verify-yamnet-obs-audio.cpp` probe against libobs and run with
MODULE DATA LIB MODEL. Set SOUND_YAMNET_DEVICE=auto to test fallback
on this environment, or SOUND_YAMNET_DEVICE=gpu and
SOUND_YAMNET_EXPECT_GPU_FAILURE=1 to verify dry error handling on a
known-unsupported GPU configuration.

## Generation-safe recovery (2026-11-27)

Every classifier, resampler, partial window and published result
belongs to one capture epoch. A control change or overflow invalidates
that epoch immediately. The callback acknowledges the new epoch before
the worker discards the old unread ring prefix. Only the consumer
advances the read cursor; no cross-thread queue reset or mutex occurs
in the audio callback.

Both top-category and per-category scores are unavailable while the
generation is invalid or until a new one-second classification completes.
The category inventory is cleared on reload or fault; ordinary 2.5-second
expiry keeps the current model's category names available. Window and
dropped-sample counters remain cumulative across attempts.

A worker that has initialized successfully may retry up to three times
after a fault, with 250/500/1000 ms backoff. Thirty successful one-second
windows replenish the retry budget. An initial configuration failure
waits for correction. Changed settings or **Retry models** reset the
budget and reload the assets. The separate read-only
`sound_yamnet_recovery_v1` reports cumulative automatic retries.
The existing read/list/score/device procedure signatures are unchanged.
Explicit GPU selection never silently falls back to CPU; Auto retains
its existing GPU-then-CPU behavior.

The public Linux runner includes reload, real queue overflow and stale
score tests at both rates, plus retry exhaustion and initial bad-path
correction. Use SOUND_RT_PROBE=1 for Linux/glibc callback instrumentation.
The tests use actual pinned MediaPipe/YAMNet assets and synthetic audio,
not a mocked classifier. They do not establish hard-real-time timing,
GPU recovery, native library crash containment or hardware clock-drift
handling.
