# Candidate status

Initial Linux public-linux profile selects Mic DSP and YAMNet only. The
combined module was freshly built, staged and exercised by actual libobs.
The two intended registrations matched; actual mic DSP and native
classification at 44.1 and 48 kHz, stale expiry and invalid-model fallback
passed. The empty quarantine remains separately available.

Next: portable local TTS worker provisioning, then independently qualify
STT, speech-to-speech, shared signals and delayed-live stems. No feature
is promoted based solely on imported code or an earlier build receipt.
The current profile still requires a user-supplied YAMNet model and native
MediaPipe runtime. Clean-machine acceptance and distribution rights remain open.

## Expanded profile qualification

Source unchanged from initial imported runtime. Exact staged module SHA256:
4cf943f1632a7c6e86dfebc98518794483ad83bc953a9711bf130a4a5247f01c
(marker SOUND_STEM_LIVE_CANDIDATE_d7abd7732a8205f0).

- All nine expected source/filter registrations and no quarantined IDs.
- Mic DSP, YAMNet at both rates, actual CUDA HTDemucs at fixed 12-second
  delay and fail-closed silence passed.
- Actual Piper, Kokoro, Chatterbox Nano, TikTok and Edge speech, unified
  switching, retention, 673-byte playback and native options passed.
- Both Whisper rates and native Parakeet inference passed. Parakeet
  48 kHz passed; 44.1 kHz initially failed the aggregate check despite
  successful inference and backend switching. The original tail-only
  failure log did not retain the lifecycle assertion result. Identical
  44.1 kHz rerun against the same binary passed, including lifecycle.
  Cause unresolved: do not treat retry as a fix or claim a clean full run.
- Native Nemotron and both OBS rates passed independently on the exact
  same binary, including partial updates, final captions, missing-model
  recovery and switching to/from Whisper at 48 kHz.
- Direct Nemotron-to-Piper relay passed all ten behavior checks;
  independent native OBS client signal test passed all twelve checks
  with actual speech, YAMNet scores and default cloud refusal.

All feature groups have passing observations on the same binary, but
the complete uninterrupted suite did not pass. Keep expanded profile
opt-in until the intermittent Parakeet failure is understood and a clean
full run is obtained. The simpler public-linux profile remains available.
The restored test harness requires explicit asset paths and now prints
lifecycle assertions even if the final 40 lines omit them. No runtime
implementation or installed OBS plugin changed.

The exact staged binary and raw diagnostic logs are retained locally
under the ignored build-public-combined directory, not in Git. No
public release, worker provisioning, clean-machine verification or
redistribution approval is implied.

## Native TikTok client

Supersedes the earlier localhost-proxy TikTok transport. Both dedicated and
unified sources use native libcurl HTTPS to the existing third-party Ottsy
endpoint; base64 MPEG decoding, mono float PCM conversion and clocked OBS
playout happen in the plugin. No Python server, localhost port 8790,
FFmpeg executable or temporary audio files are needed for this backend.
FFmpeg shared development/runtime libraries are required for TTS builds.
Cloud transcript consent, cancellation, voice selection and fail-closed
behavior are retained. Cloud availability is not guaranteed.

Verified on Linux with actual remote synthesis: dedicated source produced
96,738 nonzero OBS samples, 2.5 seconds of playback, no bad samples. A
connect/execve trace confirmed HTTPS port 443 and no port 8790 connection
or FFmpeg/Python child. Expanded module registered all nine sources and
passed the real unified five-backend switching/cancellation test. Offline
MP3/JSON validation, UTF-8 chunk boundaries, existing WAV/text regressions,
and empty quarantine passed. The complete unrelated STT/stem acceptance
suite was not rerun for this change; its previous intermittent Parakeet
failure remains open. No live installation or public publication.

## Single TTS source for public release

Supersedes the earlier nine-registration profile. The expanded candidate now
registers exactly five sources/filters. The four development-only TTS source
IDs are no longer registered, and their specialized callbacks and signal
discovery aliases are removed. All five engines live inside LLL.sound TTS.
No migration or hidden legacy registration is provided for the experimental
standalone IDs. The private development repository remains the historical
reference.

The registration regression failed against the old nine-entry binary and
passed against the new five-entry binary. Individual real OBS playback
checks passed through the single source for Piper (including monitored text,
deduplication and cancellation), Kokoro, Chatterbox and native TikTok.
After removal of the last obsolete property fallback, the final expanded
binary passed the seven-switch five-backend real playback sequence,
cancellation/recovery and independent signal-client end-to-end test
(including native Nemotron, YAMNet, Piper and cloud consent). The empty
quarantine also passed. The entire STT/stem qualification was not rerun.

## STT live-tuning lifecycle candidate (2026-11-22)

The expanded candidate now separates model/runtime setting changes from live
capture and caption tuning. Energy threshold, silence/maximum phrase timing,
caption hold and target, initial Whisper prompt, and muted-audio policy update
the worker's settings without tearing down the loaded STT model. Backend,
model/runtime paths, language and other session parameters still take the
existing fail-closed reload path. A dedicated zero-model C++ policy regression
test runs within the STT aggregate verification script.

Evidence on the unchanged historical staged module: the newly added real-OBS
hot-update assertion failed at 48 kHz (SOUND_STT_HOT_UPDATE_STABLE=0) while
recognition, caption routing and baseline lifecycle checks passed. The freshly
built expanded module passed the same actual Whisper/JFK fixture and OBS
lifecycle harness at both 48 kHz and 44.1 kHz, each with
SOUND_STT_HOT_UPDATE_STABLE=1, SOUND_STT_REAL pass=1, and missing-model
recovery. The standalone config-policy test and existing STT capture test
passed; expanded module built; zero-effect quarantine/registration check
passed. These tests used temporary/ignored staging, without replacing the
installed OBS plugin or changing the old qualified stage.

This is narrowly verified Whisper hot-tuning behavior, **not** the complete
expanded-profile acceptance suite. Parakeet's intermittent 44.1 kHz issue,
Nemotron model-backed hot tuning, sustained live OBS use, and clean-machine
qualification were not revalidated by this pass. Keep expanded features
opt-in and maintain all existing release/distribution gates.

## STT event-driven worker wakeups (2026-11-22)

Replaced the STT worker's 4 ms empty-queue polling with a Linux nonblocking
eventfd wakeup. The real-time audio callback only increments a lock-free
sequence and writes an eventfd notification if the worker is parked (no
mutex or allocation). Control changes, filter attach/detach, mute epochs,
capture overflow, and shutdown also wake the worker. The consumer snapshots
the sequence before checking the queue and rechecks it before parking so
a notification cannot be lost between the empty check and the wait.
Timed wakes are retained for finalizing an utterance after a stopped audio
source and for clearing caption-hold expirations.

A new standalone test exercises no-data timed waits, notification-before-
wait races, sleeping worker wakeup, burst coalescing, indefinite shutdown
wake, and 5,000 SPSC queue handoffs. It passes and is wired into
the combined STT verification script. Existing STT config-policy and
capture tests pass, and the expanded module builds successfully.

Real isolated libobs/Whisper integration against the *restaged final build*
passed at both 48 kHz and 44.1 kHz: hot updates preserved captions,
speech-only fragments finalized after the source stopped emitting audio,
muting and disabling still blocked transcription, invalid models failed
closed, and model recovery succeeded. The zero-effect quarantine build
and module-registration test also passed. No user OBS installation changed.

The heavy-model tests are **not clean qualifications**. Parakeet at 44.1 kHz
and Nemotron at 48 kHz each missed the harness's initial ten-second
ready-status window (ready=0, overall test exit=1). Both later successfully
recognized speech; Parakeet passed the new stopped-source/hot-update
checks and its return switch from Whisper, while Nemotron passed streaming
partial updates and its Whisper switch. This is consistent with a slow or
intermittent initial readiness path, but the cause has not been isolated;
do not label either test green or the pre-existing Parakeet lifecycle
issue resolved. Full expanded qualification, measured worst-case callback
latency/idle-CPU improvement, installed OBS behavior, and clean-machine
acceptance remain unverified. Expanded features stay opt-in.

## STT cold-start readiness investigation (2026-11-22)

Investigated the expanded STT harness's false initial-readiness verdict
with exact model/runtime evidence. Hugging Face identifies
`litert-community/parakeet-tdt-0.6b-v3` as a LiteRT-LM/MediaPipe
`.litertlm` ASR asset; Nemotron's `nvidia/nemotron-3.5-asr-streaming-0.6b`
is a distinct native GGUF-backed streaming ASR path in this project.
The upstream LiteRT-LM documentation explicitly notes multi-second
synchronous model initialization; this is not evidence that Sound's own
startup necessarily meets a particular latency budget.

Added actual per-model wall-clock initialization telemetry to the worker,
plus a startup status/latency record in the libobs harnesses. The baseline
Parakeet 44.1 kHz run read `status=loading` at 10,030 ms (ready=0),
then recorded a **successful 12,625 ms model load**; the rest of the
actual recognition, hot-update, stopped-source finalization, invalid
model/recovery, and Whisper/Parakeet switching all succeeded. Subsequent
model reloads in that process measured 516 and 355 ms. The old harness
reported overall pass=0 solely because the initial 10-second wait elapsed
before the first successful load.

Functional Parakeet readiness now has an explicit measured 20-second
ceiling in the real-OBS harness, while its **original 10-second startup
performance target remains separately reported** as `within_10s`.
The subsequent Parakeet run reached ready after 561 ms and passed the
full 44.1 kHz real OBS test (including the newly-added stopped-source
check and both backend switches). The warm run must NOT be treated as
proof that cold-start latency improved or that the intermittent Parakeet
44.1 kHz lifecycle problem is eliminated.

Nemotron at 48 kHz was not reproducibly slow: a separately instrumented
run loaded in 5,990 ms, reached ready in 5,998 ms under the **unchanged**
10-second timeout, and passed the real OBS streaming/partial captions,
invalid-model recovery, and Whisper/Nemotron round-trip test. The
earlier 10-second initial-readiness miss still requires reproducible
controlled cold-start timing before attributing a definitive cause.
No model, backend, loader, installed OBS plugin, or remote repository
was modified during this investigation.

## Hardware-adaptive Nemotron inference (2026-11-22)

Removed the Sound-specific CPU-only barrier on the native streaming Nemotron
GGUF integration. The runtime CMake build now enables CUDA automatically
when a usable CUDA compiler is detected, or builds CPU-only when CUDA is
unavailable/disabled. CPU-only builds remain supported with
`-DSOUND_NEMOTRON_CUDA=OFF`. Runtime selection defaults to the upstream
`TRANSCRIBE_BACKEND_AUTO`, with explicit `cpu` and `cuda` choices
in OBS; explicit CUDA does not silently return CPU. A v2 runtime entrypoint
adds device selection and reports the actual backend without changing the
original ABI-1 entrypoints. On automatic initialization/stream-start failure,
Sound retries the CPU path once. The old CPU runtime still opens under
automatic mode. The arbitrary 12-thread CPU setting ceiling is replaced
by the machine's reported hardware-concurrency limit.

**Build evidence:** pinned `transcribe.cpp` version 0.4.1,
NVIDIA RTX 2070 SUPER (compute capability 7.5), CUDA 12.0, GCC 12,
`CMAKE_CUDA_ARCHITECTURES=75`, `GGML_CUDA_FA=OFF`,
`GGML_CUDA_NCCL=OFF`. The isolated 140 MiB CUDA shared runtime linked
and exposed only `sound_nemotron_*` native symbols (no exported GGML or
transcribe engine symbols). The CPU variant also built.

**Real model evidence:** the existing 533.4 MiB Q5_K_M Nemotron model
loaded on **CUDA0** and passed the native streaming fixture twice, with
encoder right-context settings 0, 6 and 13; it returned the expected
recognition text. Native fixture processing measured **555–717 ms on CUDA**
vs **1790–1812 ms on CPU** in the independent measured runs (not
end-to-end OBS latency). Auto chose CUDA0 on this machine, explicit CPU
on the CUDA-enabled runtime chose CPU, and a CPU-only runtime rejected
an explicit CUDA selection. These measurements are machine-/cache-specific.

**Actual OBS:** isolated staged plugin and CUDA runtime together, no
custom runtime override in the test, explicit CUDA selected in the real
OBS filter. At **48 kHz**: `SOUND_STT_NEMOTRON_LIVE pass=1`,
`SOUND_STT_LIFECYCLE off=1 missing=1 recovered=1`,
`SOUND_STT_NEMOTRON_SWITCH whisper=1 saved=1 returned=1 invalidated=1`,
`SOUND_STT_REAL pass=1`, ready in **483 ms** and actual device **CUDA0**.
At **44.1 kHz**: real streaming `pass=1`, lifecycle off/missing/recovered
all 1, `SOUND_STT_REAL pass=1`, ready in **462 ms** and actual device
**CUDA0**. Both rates preserved unchanged audio and caption routing.
The harness separately reports the original 10-second performance target
and a bounded 20-second functional startup deadline. CPU real-OBS 48 kHz,
STT configuration and wakeup tests, and zero-effect quarantine also passed.
A separate native CPU build passed recognition and right-context settings.

A CUDA-linked build needs compatible NVIDIA driver/CUDA shared libraries
available at runtime; it is not a zero-dependency GPU binary. CUDA,
CPU, and model assets are not redistributed by this checkpoint. Full
expanded profile acceptance and clean-machine packaging remain separate
qualification tasks. This test modified only the ignored isolated
`build-public-expanded/hot-test-stage` candidate, **not the normal
installed OBS plugin**, and did not publish or push a release.

## Unified STT session and property layout (2026-11-24)

Expanded local candidate SHA256: a4f419e78b0038fd987f3f6804336e8124962b29b12f829187666132b3f58bbd.
The same filter ID and stored model/device keys are retained. A common
worker-owned backend handles loading, native streaming or phrase finalization,
reset and cancellation checks. Active-engine-only reload policy and common
model/device/caption property order are covered by regression tests that
failed before the implementation and passed afterward.

On the exact staged binary, actual 48 kHz OBS recognition, unchanged audio,
caption routing, invalidation, missing-model recovery and switching passed
for Nemotron CUDA -> Whisper -> Nemotron and Parakeet CPU -> Whisper ->
Parakeet. Nemotron published 13 partial updates; its initial ready time was
461 ms. Parakeet initial ready time was 15046 ms, above the 10-second target
but within the 20-second functional deadline. Capture, wake and config
policy tests passed. These results do not establish Whisper CUDA or
Parakeet GPU support. Earlier pending STT device-selection work is included
in this checkpoint; unrelated YAMNet and TTS worker edits remain separate.
Full expanded acceptance, 44.1 kHz retest and fresh-machine installation are
not qualified by this checkpoint. Model catalog, live telemetry UI, optional
retention and improved VAD are still open.

## Native audio diffusion source (2026-11-24)

Opt-in six-effect local candidate: df99726e9fb514d29ad88dd037c2188f1b7866990978c312f1f3845668f8d956.
Small-Music Q4_K_M/F16; real CUDA generation and timed stereo PCM playout
passed at 48 kHz (95983 captured frames for two seconds) and 44.1 kHz
(88200 frames), replay without re-inference, cancellation after sampling
progress, missing-model recovery, disabled-source rejection. Exact six-entry
registration and zero-entry quarantine passed. The same module passed
real overlapping diffusion playback and recognition with Nemotron CUDA
and Whisper CPU, including expected transcript/caption routing and dry
audio checks. No inference occurs on the OBS audio/render callback.
Runtime and model provenance: docs/sa3-native-qualification.md.
No full expanded acceptance suite rerun; pending unrelated MediaPipe/TTS
work remains uncommitted. Small-SFX, audio-to-audio in OBS, continuous
music and arbitrary GPU plugin coexistence remain unqualified. The
existing expanded preset intentionally still requires explicit diffusion
enablement.

## Optional Zipformer and rolling captions (2026-11-24)

Tested staged six-effect module SHA256: 2c42de01fa8506e61d82d426c5875efd347de97ba0058e6df0b73ca0afeb6ff9.
Pinned Sherpa v1.12.40/ORT 1.24.4 runtime, pinned 43.65 MB English
int8 model, CPU only. Real native fixture decode 158 ms, context 294 ms;
OBS startup 341 ms at both 48 and 44.1 kHz, 30 distinct partial changes
in each expanded caption test. Real recognition, unchanged audio,
caption history, transcript isolation, lifecycle and hot-update tests
passed at both rates. Four-engine layout, config policy, history tests,
runtime checksum/license staging and default quarantine passed.
The tested module predates a packaging-only CMake reconfigure with a
different generated build marker; the new packaging rule was checked
separately. Full expanded regression and switching/coexistence across
all four engines remain unverified. OBS was open, so installation was
not changed. See docs/zipformer-stt.md.

## SA3 continuous, transformation and conditioning (2026-11-24)

Tested staged six-effect module SHA256: 54ea30cede47afa42f9b5634c89117517fa2fd1059c72a0888116bda16532768.
Small SFX model files verified against their pinned Hugging Face revision
(the shared T5Gemma weights are reused). Real OBS generation, SFX,
WAV transformation, 48 kHz source capture/transform, extension,
native prompt and LoRA automation with a nontrained rank-2 fixture,
invalid adapter and WAV rejection, and output-duration bounds passed.
Sight fresh/stale ABI fixture conditioning passed; live camera inference
was not tested. Actual Zipformer transcript conditioning and recognition,
caption routing and rolling history passed. Continuous three-boundary
playout passed at both rates: 848143 frames at 48 kHz (max timestamp
gap 354167 ns) and 779247 frames at 44.1 kHz (zero gap); no invalid or
out-of-range captured samples, no failed segments, stop verified.
Manual generation/replay/cancellation/recovery/disable regressions
passed at both rates. Six-effect registry, four-engine STT properties,
runtime hashes and default quarantine passed. Full expanded acceptance,
long-duration soak, live YAMNet and Sight conditioning, trained adapter
quality, and inline diffusion filter remain unqualified. Source capture
keeps input dry and publishes separate generated audio. OBS was open:
no installation replacement or remote publication.

## SA3 session telemetry and graceful finish (2026-11-24)

Installed preceding SA3 + Zipformer checkpoint 029a95d before continuation.
Backup and installed hash recorded in the preceding stage's
installation-record.txt. No scene settings were modified.

Core session module SHA256: e08dbcee475a23772eaede35900152c1cf4a2b0ceffb862614936e89de313b9a.
A real CUDA 48 kHz test completed ten continuation boundaries and then
finished the current segment at 66000 ms. Last successful generation
1565 ms; max captured timestamp gap 354167 ns, no invalid/out-of-range
samples, no failed segments or underruns. The session status test
verified lookahead readiness, generation activity, monotonic elapsed time,
and graceful finish. All individual focused SA3 regression checks passed:
operations, capture, prompt/LoRA lanes with test adapter, Sight fixture,
24 s continuous sessions at both rates, and original manual generation,
replay, cancellation, missing-model recovery and disabled-source checks
at both rates. The aggregate shell runner returned an EOF error at its
end because its source was edited to add a refresh test while it ran.
The revised script passes bash -n; do not treat the aggregate exit as
a successful complete invocation.

Final UI-refresh-only rebuild SHA256: 4d328e3044ae898c2aede075b3be2f9489433c79e81391e3fb2c0744ddec6468.
Retained libobs property list test reproduced unchanged status text
before the correction and passed after the correction. The final module
also passed six-effect registry, all four STT property layouts and default
quarantine. The core real-model suite ran against the preceding core
module, not this final UI-only rebuild. The status panel still requires
manual refresh; finish is a segment boundary, not beat detection. A
minute-scale soak is not indefinite stability. Full expanded acceptance,
real Sight/YAMNet conditioning, trained LoRA quality and inline audio
diffusion remain unqualified. No remote publication.

## Optional native Audio Diffusion dock (2026-11-24)

Dock-enabled module SHA256: da695ff43d0ff43c595d22407f7a41032f48c53f22c05db2301f2edb4965975d. Separate SOUND_DIFFUSION_DOCK option,
no old Tools menu, no additional source registration. Real Qt/libobs
harness and CUDA model passed discovery, UUID selection, draft/apply,
generation, live status, source isolation and removal. External-edit
conflict and dirty-switch blocking also passed via the repeatable dock
runner. Isolated actual OBS 32.2.0 startup registered the dock and shut
down. No manual interaction with the frontend's dock menu was tested.
Six-effect registry and default quarantine passed. The previous
full-model SA3 regression was not rerun for this UI-only checkpoint.
Advanced source properties, adaptive buffer sizing and indefinite
continuous soak remain outside this dock pass.

## Optional adaptive diffusion reserve (2026-11-24)

Verified staged module SHA256: 774b2dcce256df0560bb41a1080cc01d6861f95aa3c9f8d7efbb8f14a3a192a1.
Two to three ready clips, decaying generation wall-time high-water policy,
single ordered producer, bounded queue, opt-in source setting and dock
draft/apply control. The deterministic policy test exercises expansion
to three clips and decay to two. Actual CUDA adaptive 24-second sessions
passed at both 48 and 44.1 kHz with 12000 ms prefill/reserve, zero
underruns, no invalid samples, and graceful finish. At 44.1 kHz a second
SA3 source completed a 60-second 32-step generation during playback;
the reserve target remained two clips (three-clip growth is policy-tested
only). Prefill cancellation discarded queued audio and subsequent manual
generation recovered with 95983 captured frames at 48 kHz. Dock setting
isolation and conflict tests passed. The focused SA3 regression runner
completed with exit 0, including operations, capture, automation,
Sight ABI fixture, continuous and manual paths at both sample rates.
Six-effect registration and quarantine passed. No guarantee against
arbitrary GPU contention, sustained slower-than-real-time generation,
or perceptual seams; full expanded Sound acceptance and indefinite soak
remain outstanding. The adaptive setting applies at the next start and
adds prompt-to-playback latency. No change to steps or quality settings.

## Native SAME-S latent morph (2026-11-24)

Tested staged module SHA256: e2dd86cd5966d45a1294e4f4bc0fe1e3df0c55a600b16eee51d551e6997fb02e.
Native isolated helper SHA256: 546850f3f12d71ff11dfdf57677bf33e9fad6b556e18314e1ede01b6f01e429c.
Opt-in SOUND_SAME_SOURCE_DIR builds from the existing clean pinned
sa3.cpp/GGML checkout, reuses the private CUDA GGML runtime, and
verifies the existing Small-Music SAME-S F16 weight checksum at each
invocation. No new model downloads or ONNX dependencies. The helper
supports fixed and ramped latent blends of equal-length stereo
44.1 kHz PCM16 WAVs, 8192 samples through 12 seconds. CPU and CUDA
outputs on two distinct 2-second fixtures correlated 0.999888;
endpoint waveform correlations were 0.994738 and 0.941242.
Latent midpoint differed from the PCM crossfade of decoded endpoints
by 0.056186 RMS; the ramp differed from fixed by 0.092163 RMS.
The 12-second CUDA fixture took 167 ms to encode both inputs and
41 ms to decode, excluding model verification and loading.
The 2-second qualified helper run took 80 ms / 8 ms on CUDA and
351 ms / 177 ms on CPU. Actual OBS morph, replay, missing WAV rejection,
cancellation during the CPU worker and retry passed at 48 kHz.
Standalone helper rejects existing output and invalid model.
CMake packaging, relative runtime lookup, six-effect registry,
original manual generation/cancellation/recovery at both sample rates,
dock regression, and default quarantine passed. The repeatable morph
acceptance runner passed. The full expanded Sound suite, adaptive
continuous soak with simultaneous morph, manual GUI interaction,
subjective morph quality and live inline latent processing are not
qualified. No changes to existing SA3 model weights or scene settings.

## Native SAME-S latent texture operations (2026-11-24)

Staged module SHA256: 5b009e3bb4940e4b62dd442290a83d2690b6b47ecbe5859a8ccc9ecfa269fcfc.
Helper SHA256: 73363e7217d8ee297cc69e064b01c161f1994ed27df5d7cacbc138584bca5b9c.
Single-input freeze, centered temporal smear, and contiguous latent
grain repetition reuse the pinned SAME-S autoencoder, shared inference
gate and isolated worker. No additional runtime or weights. Deterministic
frame indexing, zero-amount reconstruction, distinct output, exact
2-second duration, and CPU/GPU smear agreement (0.002463 RMS difference)
passed. Real OBS playback at 48 kHz passed all three modes with no
second WAV; existing A/B morph, cancellation and recovery passed in the
same runner. Standalone CUDA 12-second fixtures passed exact duration
and amplitude checks, with single-input encoding 94–96 ms and decoding
39–43 ms excluding model load and verification. Six-effect registry
and Qt dock regression passed. No subjective listening, continuous
live capture/latent processing or expanded Sound acceptance in this
checkpoint. Separate pre-existing YAMNet delegate and TTS provider
changes were inspected and preserved outside this commit; their actual
device qualification and production configuration still require
separate reconciliation.

## Resident SAME-S texture worker (2026-11-24)

Staged module SHA256: 10e7c301ce6261cd14347bdf5b3100473b8668bf72546c5dac43e5d834d68343.
Helper SHA256: 78bcc539f55989a605cb704768ea1c367dbcba5f1d3d880264f1cafd13a212cc.
Single-input textures use one retained model/graph-pair context across
requests, bounded reusable host buffers and private local socket PCM
transport. One warm context across sources; another source uses the
one-shot fallback. Idle context may overlap other source GPU allocation.
Actual OBS texture/morph runner passed reuse across three modes, Stop,
restart, eight-second idle release, CPU request cancellation and CUDA
recovery. Standalone CPU/CUDA tests passed changed input, identical
repeat output, geometry replacement, invalid mode/nonfinite sample
rejection and clean EOF. Warm 2-second CUDA compute took 13–14 ms in the
initial run. Final CUDA short-block test completed 60 changing 16384-frame
requests with one graph pair for that geometry: median wall 8.6 ms,
95th percentile 14.3 ms, max 16.5 ms. Observed warm worker GPU allocation
368 MiB. These are compute/transport results, not end-to-end live latency.
Manual original SA3 generation/replay/cancel/missing/recovery and disabled
checks passed at both sample rates; Qt dock, six-effect registry, and
quarantine passed. No new weights or runtime. Streaming capture, decoder
context/overlap handling, seam quality, cross-source contention test and
full expanded Sound acceptance remain open. Existing separate YAMNet/TTS
work is excluded from this commit; the staged module was built in the
working tree containing the pre-existing YAMNet change.
