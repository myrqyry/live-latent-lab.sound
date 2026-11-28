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

## Shared neural audio worker wakeups (2026-11-27)

The Linux STT eventfd mechanism now lives in `framework/rt_wake.hpp`;
STT retains its compatible alias. DDSP, native RVC and buffered stems
signal their workers after queued input reaches a useful processing
threshold (one DDSP hop, at least one RVC hop, or one stem window).
Shutdown and pipeline faults also signal; DDSP settings corrections wake
its initial model retry. A bounded 100 ms watchdog remains in the newly
converted workers. The existing queue ownership, fixed delays and failure
policies are unchanged; this is not automatic fault recovery.

The standalone wake/readiness test covers notification races, timeouts,
stereo queue handoffs and threshold readiness at 44.1/48 kHz, and is wired
into CI. The final staged seven-effect build passed the 36-second real
Whisper/DDSP/CUDA RVC chain: 3,600 callbacks per filter, zero observed
allocator/lock/wait violations, 1,800 DDSP hops, 70 RVC hops and zero STT
drops. Dispatch p99 was 0.866 ms and maximum 6.235 ms, with no 10 ms
overruns in that run. An earlier per-packet notification prototype had
three wall-time overruns; the threshold-gated implementation supersedes it.
These observations are not a controlled before/after benchmark or a
guarantee against scheduler preemption.
Final 44.1 kHz isolated real-model DDSP and RVC tests also passed
(595 and 16 hops respectively); the affected model-free transport and
wake tests passed again.

Actual CUDA HTDemucs with a synthetic stereo speech/tone fixture passed
the 24-second libobs acceptance test: 1,152,000 frames, correct initial
silence, distinct stereo vocal output, and 2,400 measured callbacks with
zero allocation/free/lock/wait violations. The earlier weak-vocal music
fixture missed the existing energy thresholds both before and after the
first wake change, with identical measured audio energies. The fixture
was replaced, not the acceptance thresholds.

### Remaining wake migration (2026-11-27)

YAMNet, DeepFilter and live SAME-S now use the same Linux eventfd
implementation, completing the seven targeted worker paths. YAMNet
notifies on capture publication/overflow and shutdown. DeepFilter
notifies for a complete 480-sample chunk or an acknowledged generation
awaiting worker reset, as well as model changes and shutdown.
Live SAME-S notifies on input, invalidation and local cancellation,
retaining a 20 ms watchdog for externally removed/disabled input and
the existing two-second stalled-input policy. Non-eventfd builds retain
bounded polling; that alternative header path is exercised in CI, but
native Windows/macOS builds were not qualified.

Verification on yuzu-mint:
- Real pinned MediaPipe/YAMNet at 44.1 and 48 kHz: classification,
  unchanged stereo dry, 521-class signal catalog, missing-model behavior,
  stale expiry, zero drops. 828 instrumented callbacks, zero observed
  allocator/free/lock/wait violations.
- DeepFilter actual ONNX Runtime transport with deterministic gain-model
  fixtures: model reload, recovery and callback tests passed; 82 measured
  callbacks, zero violations. This is not trained-model quality evidence.
- Live SAME-S real CUDA model: both sample rates, stall/restart, and
  40-second soak passed (106 blocks, zero drops/underruns on soak).
  Competing generation initially missed its completion deadline despite
  uninterrupted live output; pre-change baseline and unchanged post-change
  repeat passed. Repeat produced 37 blocks, zero drops/underruns and a
  completed competing job. Cause remains unresolved; do not claim this
  is a fixed regression or a controlled performance comparison.
- Across the new SAME-S runs, callback probes observed no allocations,
  frees, locks or waits, and no callback exceeded 10 ms.
- Shared wake, generation-readiness and transport tests passed; the
  fallback polling header was compiled/exercised separately on Linux.
  Seven-effect staged build SHA256:
  36786fb1d6142db21acfa1c27855ef92a72687aaff4ed1d0821a8d4b1db4bb98.
  DeepFilter was built and tested separately.

The system partition was almost full and other OBS processes were
running; existing runtime directories were reused in the isolated
staging tree. No user profile changes or release promotion. Full expanded
release acceptance remains outstanding.

## DDSP generation-tagged recovery (2026-11-27)

The worker and callback now exchange generation acknowledgements before
discarding their respective SPSC queues. The dry delay line never
resets after activation; the wet timeline starts a fresh 200 ms prefill
against the current capture position and ramps in over 10 ms. Obsolete
worker results cannot enter the newly acknowledged generation. On a
fault, the transition to dry retains the prior immediate-fallback
behavior (not guaranteed clickless).

Model/runtime changes are now live, including correction after a bad
initial load. Worker faults use 250/500/1000 ms backoff and rebuild the
native runtime, all decoders, analysis, synthesis and resampler state.
Three failed/repeated recovery attempts exhaust the budget; 1500
successful hops replenish it. Explicit Retry models or changed paths
reset the budget. Initial configuration errors do not auto-retry.
The existing status procedure is preserved and a separate
sound_ddsp_recovery_v1 reports cumulative automatic retries.

Verified on yuzu-mint, staged seven-effect Linux module SHA256:
866b6840d7bc2d019068e1dc66c671bd51df9e6a41ad136ad235a8ca2dd1fe14.

- Model-free recovery tests cover both rates, in-flight obsolete
  results with deterministic thread barriers, repeated invalidation,
  sample-level alignment with uneven packets and retry-budget policy.
  Allocation guards and UBSan passed. TSan failed to initialize on
  this host (unexpected memory mapping), not a race-detector pass.
- Full DDSP verification passed with the actual pinned CPU LiteRT
  models and Linux callback probe enabled: both rates, existing
  timbre/morph/pitch/dry/silence behavior, real burst-induced fault
  recovery (one retry at each rate), live invalid/valid model reload,
  initial invalid path correction, three retries followed by dry
  fallback on the fourth forced fault and manual Retry models recovery.
  The instrumented callback tests passed their allocation/free/lock/
  wait assertions. No test threshold was relaxed.
- Concurrent real Whisper/DDSP/CUDA RVC chain passed all 3600 packets
  with zero STT drops, 1800 DDSP hops, 70 RVC hops and zero observed
  callback allocation/free/lock/wait violations. Dispatch p99 was
  1.249 ms; two packets exceeded 10 ms wall time (maximum 31.127 ms).
  Callback CPU maxima were below 30 microseconds, consistent with
  scheduler interference rather than measured blocking in the callbacks.
  This is not proof of a hard real-time deadline guarantee.
- Existing model-free wet-block, RVC-window, stem pipeline, wake and
  DeepFilter transport tests passed. This does not qualify recovery in
  RVC or stems; their longer windows and failure policies differ.

## Native RVC generation recovery (2026-11-27)

DDSP and RVC now share the same generation-tagged fixed-delay mono-wet
transport. RVC retains its 1500 ms delayed stereo dry, 750 ms first
capture, 500 ms hops, existing crop and SOLA behavior. Wake readiness
now uses the actual first-window threshold and resets on recovery.
The child, window and SOLA history are rebuilt on faults or settings
revision; old output is discarded before the fresh wet prefill.
The helper's cold-transfer path can warm up while the audio generation
is faulted, but both cold and live IPC observe cancellation and revision.
The previous child is reaped before releasing the exclusive CUDA slot.
Voice/runtime/checkpoint/index/speaker settings now reload live; the
legacy ONNX guard is retained. Automatic retries use the existing
worker-owned budget, replenished after 60 successful hops; Retry models
or changed paths reset the budget. After 30 s without input the worker
releases its session and waits for actual audio activity.

Verified on yuzu-mint using staged seven-effect Linux build SHA256:
530da26d9be68158e4413e9c63f2a584ac1db0b28a7875d7ad761434a296d54d.

- Model-free RVC recovery at both rates with uneven packets, exact dry
  alignment, new capture origin, old result rejection, superseded restart,
  and the correct first and subsequent wake thresholds. Allocation guards
  and UBSan passed. DDSP recovery, pipeline, wet-block, RVC window and
  wake tests passed after extracting the common transport.
- Real CUDA RVC ordinary OBS audio at both 44.1 and 48 kHz passed
  (16 hops each). 48 kHz live helper-path reload passed (44 hops);
  the pre-change binary failed the same fallback test. Actual isolated
  child crash recovered on the first retry (33 hops). Actual 30-second
  idle/resume recovered without consuming a retry (34 hops).
- Missing-helper safety and legacy ONNX guard with valid native
  assets passed; neither allowed unintended wet output. DDSP real
  48 kHz hot reload on the shared transport passed (177 hops).
- RVC probe runs across ordinary conversion, reload, crash, idle and
  legacy guard measured 11,400 callbacks, zero observed allocator/free/
  lock/wait violations and no callback above 10 ms. These are targeted
  observed results, not a hard-real-time guarantee.

Limitations: recovery runtime tests use the pinned packaged CUDA model
at 48 kHz, not every custom checkpoint/index/speaker permutation.
RVC retry exhaustion shares the unit-tested policy but was not
independently forced through four GPU crashes. The 1500 ms dry delay
is measured in received samples, not absolute time across source gaps;
pre-gap dry history is retained. Fault transition is not guaranteed
clickless. Entire native helper protocol wrapper and full expanded
release acceptance were not rerun in this pass. No installation or
publication. System disk remains critically full; existing runtime
assets reused in isolated staging.

## YAMNet generation recovery (2026-11-27)

The native MediaPipe classifier now uses an acknowledged capture epoch.
The callback remains bit-preserving pass-through and does not block,
allocate or touch model state. Old ring data is discarded only by the
consumer after acknowledgement; the resampler, partial classification
window and classifier are rebuilt on the worker. Published labels and
per-category scores are invalidated immediately on overflow or settings
change. The category inventory remains discoverable after ordinary expiry
but is invalidated on reload/fault. Existing procedure signatures and
cumulative window/drop counters are retained.

Model, runtime and delegate reload live. Runtime failures receive the
shared three-attempt bounded retry policy; 30 successful classifications
replenish the budget. Invalid initial configuration requires correction.
Retry models explicitly resets the budget; sound_yamnet_recovery_v1
reports cumulative automatic retries. Explicit GPU selection retains
its no-fallback contract; Auto retains GPU-then-CPU behavior.

Verified on yuzu-mint with the pinned native MediaPipe and YAMNet CPU
assets. Final staged seven-effect module SHA256:
456fc354352301f633fad6d625162c60e3dbe60f5edd19c58194402f417ccdb4.

- The previous module failed the live model-path regression: an obsolete
  Sine wave score was still available. The new module rejects the old
  top label and per-category score and resumes on fresh input.
- Full isolated YAMNet-only public Linux runner passed, including exact
  registry, ordinary inference at both rates, both-rate live reload
  and forced overflow recovery, four successive overloads (three
  retries then unavailable), manual retry and initial bad-path
  correction. Native classifier and resampler used actual model assets.
- Final public-profile probe measured 15303 callbacks across those
  scenarios, zero observed allocation/free/lock/wait violations and
  no callback above 10 ms (largest observed 6.248 ms). An earlier
  combined-build ordinary 48 kHz run had one 10.641 ms wall-time
  callback despite a 28.053 microsecond CPU maximum. This is not a
  hard-real-time guarantee.
- Explicit GPU failure preserved dry on this host in the combined
  build before the final snapshot-read tightening. GPU recovery is not
  qualified. Ring, classifier capture, wake, DDSP/RVC recovery and
  DeepFilter transport regressions passed; capture UBSan passed.
- The final snapshot reads one capture epoch for consistent availability
  and status. A stale result is not a valid new-generation result.

Remaining: native-library crash containment, concurrent classifier
instances, real microphone/hardware-clock behavior and full expanded
release qualification are not established. No installation/publication.
The system partition is still critically full; reused staged runtimes.

## HTDemucs generation recovery (2026-11-27)

The live stem filter now uses an acknowledged stereo capture/output
generation. Every restart starts a fresh 12 s silent prefill, clears
obsolete queued input and output using their respective consumer
cursors, and never emits dry music. Changing the selected stem or
socket invalidates the old generation. The client reconnects to the
user-owned external sidecar with three bounded attempts, reset after
four healthy eight-second windows or explicit Retry worker/settings
changes. No sidecar process ownership was added. Requests use a single
12 s transfer cap, 20 ms nonblocking poll/cancellation checks and a
worker-reused response buffer; the existing four-second audio deadline
still governs output. A canceled socket cannot cancel a model call
already executing inside the external sidecar.

Verified combined seven-effect staged module SHA256:
4e9a3e241bf1c96fbfcb5e1bacbb95738620a2113ad7238352203dede65be859.

- Model-free generation transport at 44.1/48 kHz passed exact silent
  prefill, distinct stereo wet, queue discard and old-result rejection.
  UBSan, existing stem pipeline, shared wake and DDSP/RVC recovery
  tests passed. Socket protocol and stalled-peer cancellation passed.
- Isolated stem-only build/registration and all eight Python sidecar
  contract tests passed via tests/verify-stem-live.sh (without model).
- Actual libobs transport integration using a deterministic TEST ONLY
  protocol peer passed both same-socket automatic recovery (one retry,
  three windows, zero energy in first 20 s) and socket settings reload
  (zero energy during fault and fresh 12 s prefill, two new windows).
  These are not model-inference tests. 8,200 measured callbacks in
  these two cases, zero observed allocations/frees/locks/waits or
  >10 ms callbacks. The separate reproducibility wrapper is
  tests/verify-stem-recovery-obs.sh (syntax checked; equivalent
  individual commands executed). An unavailable real endpoint also
  produced all-zero audio for 14 s (1,400 instrumented callbacks).
- Real CUDA HTDemucs FP32, shifts 0, overlap .1, segment 4: initial
  settings-reload run produced valid wet after recovery, with zero
  energy during fault and prefill, but later 4.819 s separation
  missed the four-second audio deadline. Same-socket repeat reconnected
  and emitted real separated audio (3.417 s window), then muted on
  an 8.642 s window. Subsequent settings-reload and normal runs
  failed sustained wet acceptance under current load; logged
  10.860, 6.191, 4.658 and 4.104 s windows. Late replies were
  rejected/canceled. No timing thresholds relaxed.
- Across the four real-model runs (15,500 callbacks), zero observed
  callback allocations/frees/locks/waits; one wall-time callback
  29.627 ms, corresponding run CPU maximum 21.220 microseconds.
  Multiple existing OBS processes were active; not interrupted.
  No controlled before/after performance comparison.

Qualification boundary: generation recovery and reconnect transport
are verified; sustained real CUDA recovery and normal playback are
NOT qualified by this pass. Retry exhaustion for the actual stem
worker, native model latency under contention and hardware-clock
behavior remain to be qualified. No install or publication.

## Resident in-memory HTDemucs (2026-11-27)

Added explicit `--backend resident` to the external sidecar. The file
adapter remains the compatibility default; no automatic fallback,
download, runtime installation or service change. The new backend
loads the exact local 955717e8-8726e21a.th model (full SHA256 checked),
validates the bag, source names and sample geometry, and requires
audio-separator 0.47.0. The observed runtime was torch 2.14.1+cu130.
One model stays on the selected device; input/output PCM16-equivalent
conversion, .9 per-stem peak limit and resampling preserve the reference
signal policy without file serialization. Named stem mapping avoids
assuming library-specific source indexes. Protocol v1, 8-second
windows, 12-second silent prefill and four-second deadline unchanged.

Evidence on local CUDA FP32, shifts 0, overlap .1, segment 4, after
closing existing OBS processes:
- A/B direct inference used two different 48 kHz synthetic stereo
  speech/tone windows, one converted 44.1 kHz window and 44.1 kHz
  silence. Four named stems checked separately. All passed unchanged
  numeric thresholds; maximum difference .0000351, RMS differences
  below 6e-7, silence exactly zero. Model object and first CUDA
  parameter address stable over warmup and all four requests.
  First run medians 1.128 s file versus .360 s resident (3.13x);
  resident model load .278 s, warmup .372 s; peak PyTorch CUDA
  allocated tensor memory 385.3 MiB (not total VRAM).
- Final code A/B repeated after asset ambiguity and effective segment
  guards; all four cases passed again.
- Closed-OBS file baseline live acceptance passed (1.350/1.152 s
  windows). Resident sidecar passed actual libobs normal playback,
  automatic same-socket recovery (one retry, three windows, zero
  initial 20 s energy), and socket settings reload (zero fault and
  fresh prefill energy, two new windows). Resident live processing
  .382-.492 s across nine requests after warmup; one canceled
  response at test teardown logged BrokenPipe as expected. Model
  load counter stayed 1 across 10 windows including warmup.
  10,600 instrumented callbacks across these three runs: zero observed
  allocations/frees/locks/waits or >10 ms callbacks; maximum wall
  36.317 microseconds. The OBS stage was unchanged c9de836, SHA256
  4e9a3e241bf1c96fbfcb5e1bacbb95738620a2113ad7238352203dede65be859.
- Red tests failed before implementation. Final 12 Python tests passed,
  including PCM16 parity against libsndfile, stable named stereo
  ordering at both rates, missing/invalid/nonfinite outputs, and
  actual resident socket protocol with temp-directory/file writes
  prohibited. Stem-only wrapper/registration passed with the first
  11 tests; final twelfth test executed separately. Runner accepts
  STEM_VERIFY_BACKEND=resident; equivalent real OBS cases executed
  with existing isolated combined stage (not full expanded release).
  Reproducible direct test: tests/verify-stem-resident-model.py.

Scope: the real recovery qualification gap from the previous pass is
closed for this particular local FP32 configuration under reduced
desktop load. CPU, autocast, other quality settings, sustained music
quality, multi-client contention, long-run VRAM residency, retry
exhaustion and clean-host qualification remain open. No latency
reduction claim, no cross-window overlap-add/fanout or GPU governor.
No installation or push. System drive now has about 18 GiB free
after the user's requested cache/backup cleanup.

Next: shared multistem fanout/overlap-add and measured DSP optimization;
preserve original roadmap and real-time callback contract.

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

## Continuous live SAME-S, native YAMNet and local reader providers (2026-11-24)

Qualified staged module SHA256: 28302f1226a7731d6e1e01610314a80e083cebe69cb0cb3c7931667f002558dc.
Continuous single-input neural texture source uses 32768-frame windows,
16384-frame hops, aligned linear crossfade, bounded capture and output
queues, worker-owned resampling and a sample-count output clock. The
source preserves the original dry signal. One live resident worker may
execute concurrently with the serialized batch diffusion gate; another
live source cannot obtain a second resident slot. The initial serialized
test exhausted the reserve under heavy diffusion; the bounded concurrent
implementation passed actual 60-second/32-step SA3 contention at 44.1 kHz
with eight-block reserve (3347 ms startup, zero dropped/underrun).
Default 48 kHz four-block reserve measured 1860 ms startup; the 40-second
input run processed 106 blocks, captured 1834301 wet frames, dropped zero,
had zero failures/underruns and max timestamp gap 354167 ns. Stalled input
stopped wet output, released its helper, and explicit restart recovered.
The actual generated-music audition run passed (max adjacent sample step
0.1112; no clipped samples), with dry and wet WAVs retained in the local
qualification directory. These are objective measurements, not subjective
listening or indefinite hardware-clock/saturation qualification.
The complete focused SA3 upgrades runner passed both sample rates,
capture/automation/LoRA/Sight fixture, SFX/transform/extend and original
manual generation. Resident texture/morph runner with lifecycle, dock,
registry and quarantine passed. New live wrapper assembles the separately
executed cases; its entire wrapper was not rerun as one invocation.
YAMNet commits d37e3d3 (CPU default, Auto fallback and strict GPU error)
and reader commits df4d5bd/7562df7 (provider policy and docs) are included
in the history; no remaining pre-existing uncommitted YAMNet/TTS changes.
Real YAMNet CPU/Auto at both sample rates and explicit GPU-failure dry
pass passed. Native GPU graph init failed on this MediaPipe/model pair.
Qualified model copied without replacing files to external Audio/yamnet.
Local Piper/Kokoro policy tests, option and retention tests, direct
CPU speech and actual reader service socket passed. Existing ONNX
Runtime offers CPU but no CUDA provider. Reader service drop-in points
to the current repository; original unit/old checkout preserved.
Full legacy expanded acceptance (including online speech, stem and all
STT backends) was not rerun; the old wrapper still has obsolete counts.

## Native Kokoro CUDA reader (2026-11-24)

Pinned audio.cpp db1869d0d53c7839b79cf7c1a16d36cc20102307, custom
Kokoro composite, CUDA 12.9 / SM75, no NCCL, ABI 0.2.
Shared library SHA256 d2996ac2f14dabb7dde15a86de7e23f8fa8091a3e57dfe438ec0840aff756745.
Staged Sound module SHA256 0ec9cfc0026e580c6fa2814df003be618f9571c2d469a18012c9744784fcda2d.
Actual native CUDA short speech test: 1308–1817 ms cold including
checksum and model initialization; 113–206 ms warm on two voice/speed
requests of differing output lengths. A forced child death failed
explicitly, then a new request recovered. Actual isolated OBS
native speech: 109168 frames, 68560 nonzero, 0 invalid, speed and
cancellation/recovery passed, 3 completed/0 failed. ONNX Kokoro and
Piper regression and real socket lease eviction/invalid option
rejection passed. Native policy, existing provider/option/retention
unit tests, six-effect registry passed. Native has separate opt-in
voice-matched English and speed controls; other ONNX controls stay
on ONNX. No claim of subjective parity, immediate kernel cancellation,
full expanded suite or three-way contention with the final narrow
runtime (earlier upstream CUDA release contention passed).

## Chatterbox native Turbo CPU and Nano model recovery (2026-11-25)

Actual installed Nano previously had four missing required model blobs.
Pinned revision 71ccd1d0081b430592cea481f4307e764e07bc64 restored to the
external audio library; only broken content-addressed links changed,
with rollback receipt. Built-in and synthetic-reference speech passed
through the existing service socket. The original Nano service unit
was preserved. The unused non-meanflow checkpoint remains absent.
The upstream Turbo Q8 CUDA release benchmark succeeded (warm median
1245 ms, 1664 MiB); two custom CUDA C ABI first-request probes were
interrupted without results. The original boot journal was unavailable;
no claim of exact failure cause. Both custom builds disabled CUDA
FlashAttention despite direct T3/S3Gen attention calls. CUDA builds
quarantined; no further Turbo CUDA inference run while OBS open.
The CPU diagnostic succeeded, followed by a separate CPU-only
audio.cpp build at db1869d0d53c7839b79cf7c1a16d36cc20102307.
CPU-only shared library SHA256
a0392fface60621ed63df109a4e3fa307d0d4c7eff673834c86d48d6630791f0;
5.9 MiB, no CUDA/NCCL dependencies. Model SHA256
acf02bed2b9ac94e271a01f556526818161a28009120f2337b9e8e10035c42b6.
Separate built-in English voice CPU backend, no cloning or silent
GPU-to-CPU substitution. Four-thread runtime, maximum 128-byte input
chunks, sampling controls, existing private reader transport.
The complete focused native Turbo runner passed real CPU speech,
tagged speech, worker death/recovery, eviction, OBS playback/cancel
and recovery, native CUDA Kokoro, ONNX Kokoro and Piper regression,
real socket retention and unsupported-reference rejection. Registry
six effects, policy/options/retention/device tests passed. Staged
module SHA256 9085365bc2c0f955eff2e523432c95149bbf860af37d26c2ecb4533284c21fab.
No claim of realtime CPU throughput, subjective speaker similarity,
full expanded suite or long multi-chunk completion. CPU-only package
and runtime retained, active plugin and reader configuration left
untouched because OBS is running. See docs/native-turbo.md.

## Native DDSP Neural Timbre filter (2026-11-25)

Opt-in ddsp gate, Linux x86_64, native LiteRT 2.4.0 CPU C API and
independent phase-continuous harmonic / filtered-noise synthesis.
Official Magenta model pack plus pitch model: 12 verified files,
83,535,361 bytes; all 11 recurrent instrument models tested. Native
feature + two decoders mean 0.515 ms. Full seven-effect staged
module SHA256 89f6e75003821ff31d43fa60c7cd93fc65cdc0398001dc24c1133b10944217fc.
Real libobs at 44.1/48 kHz: 595 hops at each rate, final full worker
hop 1.136/1.092 ms, pitch 219.885/219.890 Hz from 220 Hz input,
distinct learned Violin/Flute morph and live Trumpet switch, octave
shift, noise-only output, stereo dry preservation, zero invalid
samples, zero settled silence output. Missing-model immediate dry
passthrough and corrected path recovery (85 hops) passed. Full
isolated process peak RSS approximately 218 MiB. Seven-effect
registration and default quarantine passed. Fixed 200 ms delay after
model load, initial silence at transition, mono wet/stereo delayed
dry, fallback to aligned dry on failed worker/deadline. No subjective
audition, seamless latency compensation, pause/resume discontinuity
or long hardware-clock qualification; full expanded suite not rerun.
Model files and runtime remain external to git, with per-asset
SHA256/size manifest and model/runtime licenses. See docs/ddsp.md.
OBS was open, so the new package was staged without replacing the
loaded plugin. The previous CPU Turbo checkpoint is included in
the combined build, but its reader configuration remains pending.

## Local seven-effect installation (2026-11-25)

Installed module from d9f98f9 SHA256
89f6e75003821ff31d43fa60c7cd93fc65cdc0398001dc24c1133b10944217fc,
matching the qualified stage. Verified all DDSP model/runtime
checksums and CPU-only Turbo model/runtime checksums. Existing
SA3/STT runtime trees matched and were preserved. Prior plugin and
reader service drop-ins backed up. Added a reader service drop-in
retaining native Kokoro and enabling CPU-only native Turbo.
Post-install actual libobs registration: seven effects.
DDSP at 48 kHz: 595 hops, final worker hop 1.376 ms; all audio
acceptance conditions passed. Turbo through the installed reader:
picker, controls, cancel, recovery passed; 103648 frames, 77129
nonzero, zero invalid samples, 3 completed and 0 failed.
Both reader and Nano services active. No OBS scene changes.
This supersedes the preceding deferred installation status.
No full expanded acceptance rerun, subjective audition, or
CUDA Turbo qualification. Local installation is not a release
or redistribution approval.

## Microphone DSP extraction (2026-11-25)

Deterministic microphone modeling is now owned by HyperDSP. Sound no
longer accepts the mic build gate or registers mic_modeler_filter.
Mic-only sources, engines, tests and installation scripts were removed;
the public baseline is YAMNet only, and expanded preset has four
neural entries. The six-effect combined candidate retains stem_live,
yamnet, tts, stt, diffusion (including dock), and ddsp.

The previous installed seven-effect binary failed the new six-entry
registration expectation. The new staged candidate SHA256 ca6ffc0a52d823d1cbec6c36a975111c0eb2d47b63b66a87c8e25ea8f34fe481
passed exact six-entry registration, zero-effect quarantine, rejection
of the removed mic gate, real YAMNet audio at 44.1/48 kHz with the
complete local model, DDSP native and OBS tests at both rates and
missing-model recovery. HyperDSP microphone audio passed with the new
Sound module in either load order. The full STT/TTS/stem/diffusion
acceptance suite has not been rerun; the staged SA3 morph helper and
Nemotron runtime differ from the currently installed files. No claim
of full combined qualification or installation; preserve existing
runtime files and require appropriate verification before deployment.

## Native RVC private-worker filter (2026-11-25)

Native audio.cpp RVC replaces the quarantined ONNX adapter in the
explicit rvc gate, retaining source ID stts_rvc_filter. Pinned narrow
CUDA runtime e485ca410b4ea7f503db58b4ef6bbbf5b43b7b60c145bc4f8d0c10e180b7f977,
64 MiB, no NCCL, FlashAttention retained. F16 GGUF on disk and F32
runtime weights. Private socketpair resident process; no listener.
Real C ABI cold 4957.3 ms, warm 306.6/310.0 ms. Rejected invalid
pitch, then reused the session. Actual 44.1/48 kHz OBS conversion,
16 completed hops at each rate, last 305.7/300.5 ms, stereo dry and
mono wet, zero invalid samples, no pipeline fault. One resident
session, 1.5s fixed delay, bounded capture, SOLA stitching, delayed
dry fallback. Missing-runtime and old ONNX selection dry tests
passed; default empty quarantine and combined seven-effect registry
passed. Current combined stage includes separate mic removal
(5df496d); installed module predates it, so stage not installed.
Default native voice only, no custom model/retrieval UI yet;
subjective audition, long soak, contention, A/V compensation,
in-filter worker-crash recovery, and full expanded suite remain
unqualified. See docs/native-rvc.md and docs/native-rvc-qualification.md.

## Native RVC custom voices (2026-11-25)

Existing external voice library located through its relocation notice;
17 .pth files and 14 same-stem .index pairs. Native v2 protocol and
helper support a selected local checkpoint and optional matching FAISS
index, live blend, index warmup, default-voice regression and bounded
asset sizes. Actual custom RVC v2 40 kHz synthetic-voice conversion
with 768-dimension IVF index passed the focused real OBS runner at
48/44.1 kHz (16 hops each, last 328.1/326.2 ms); private requests
with cached retrieval 320.8–322.2 ms. Registry seven effects passed.
Stage SHA256 7c81e7c70eacf94e672d7fcb603575ff489d61b7f80ce7dc3ef9780fe2ae6cd6.
New helper and local voice-library symlink preserved; existing
weights, runtime and v1 helper unchanged. No private checkpoint
paths or model files committed. Only one custom pair qualified,
no subjective audition or full regression run. Paths apply after
scene reload; no auto-pairing or live checkpoint switching yet.
OBS open, staged not installed.

## Model settings expansion (2026-11-25)

RVC V3 native protocol, four packaged voices and their embedded
retrieval indexes passed private CUDA inference; custom checkpoint/
index, nondefault RMS/protection/median settings and invalid-value
rejection passed native and real OBS tests at both rates (16 hops,
332.5/312.9 ms final hop). Speaker ID is exposed; only zero tested.
Native Kokoro CUDA seed/chunk and CPU Turbo seed/token cap/chunk
passed real native generation and actual isolated OBS playback,
controls/cancel/recovery, each three completed and zero failed.
Reader policy, options, retention and device tests passed. SA3 CFG,
distribution and tail padding passed real Small Music OBS generation,
replay, cancel, missing-model recovery and disable at both rates on
retry; first lifecycle attempt timed out, focused diagnostic passed.
Combined seven-effect registration passed. Stage module SHA256
78898414cd21b074b9ebf975b49d6e0dff23f43387527ca326bafa758e6e4f3c.
No module or reader service installation performed in this pass.
No blanket qualification of all model options or numeric extremes.
The remaining supported-control gaps and fixed contracts are listed
in docs/model-settings-audit.md; full expanded regression not rerun.

## Recognition and shared stem model settings (2026-11-25)

Whisper and Zipformer now expose advanced model decoder settings
with backend-specific visibility. Real CPU recognition passed both
host rates for both engines, hot updates, unchanged audio,
caption routing, silence, mute, disable and missing-model recovery.
Zipformer retained rolling partial captions; matching BPE vocabulary
for plain-text hotwords is not present in the pinned model package.
HTDemucs shared CLI now exposes device, shifts, overlap and segment
length. Cold first inference missed the four-second deadline;
pre-readiness warmup corrected the observed problem. Real CUDA OBS
nondefault (0/.1/4s) and default (1/.25/model default) settings
passed: two eight-second windows respectively 1.309/1.208 s and
1.366/1.241 s. Eight worker unit tests and isolated default
acceptance including missing worker silence passed. Underlying
adapter still reloads weights per call. Worker graphical controls,
other accepted gaps and full expanded regression remain open.
Combined seven-effect staged module SHA256 2756229d8f8438dc3dbd276990c377f22f49fe2e53e37e01baabcb147ae4dd08.
No installation or push in this pass.

## Source-preview preparation (2026-11-25)

Reviewed synchronized main 074fc0e and reachable Git history; basic
credential/personal-path scan found no matches, no production model
weights or shared libraries tracked. The existing 103-byte ONNX
negative fixture now carries its upstream Microsoft MIT notice.
Reconciled README with mic-free seven-effect candidate and older
installed module. Added dependency/distribution inventory and
standalone YAMNet setup, with no model or runtime bundling. The
qualified model is 4,126,810 bytes, SHA256
4d8b4a53282dc83ef04e3e7dbc4fbc98082e34e44ed798e16c3a0cdd4c584faf;
Google object metadata size and MD5 matched. MediaPipe 1.2.3
library SHA256 7ce8b23159d3fd4e8b727e35afbfb35f3d45d6bde1ec11cc01c316b3bf469f63
matches the previously verified official wheel member. Fresh baseline
CPU inference at 48/44.1 kHz: Silence .800781, Sine wave .996094,
521 category signals, zero dropped samples, missing-model dry
fallback, stale expiry, original RMS .212132/.106066. A separate
shorter cached model was rejected as an invalid FlatBuffer; not
modified or included. Staged seven-effect binary 2756229d8f8438dc3dbd276990c377f22f49fe2e53e37e01baabcb147ae4dd08
rechecked exact seven registrations in isolated libobs, not a new
seven-effect inference acceptance run.

Shared CI pin 092e14e32c65b9447bc0a572e789edcf4074ac91, Linux OBS
32.2.0 caller driver, five model-free test groups: baseline registry,
quarantine, concurrent ring-buffer, STT config and public URLs.
Verified local driver against installed OBS SDK; quarantine supports
explicit SDK path rather than accidentally requiring system libobs.
Test package strictly allows module, locale/manifest, LICENSE,
NOTICE; no model/runtimes. Source archive from committed tree only.
Hosted run and fresh downloaded SDK not yet qualified. Full
expanded suite, Parakeet intermittent failure, SA3 lifecycle retry,
cross-platform and clean-host provisioning, current GUI captures,
comprehensive source/license audit and expanded redistribution
clearance remain open. No runtime implementation, installed plugin,
worker configuration, remote branch, or repository visibility changed.

Source package follow-up: the first binary audit passed, but the
source archive was rejected by the shared auditor because three
formatter aliases were symlinks inherited from the OBS template.
Replaced only those three aliases with regular executable wrappers
delegating to the same zsh formatter, with explicit formatter names.
Original and replacement version output matched (2.0.0); all three
replacement help paths passed. No archive-policy exceptions.

## Local installation follow-up (2026-11-25)

Installed the seven-effect module corresponding to f12af01, SHA-256
2756229d8f8438dc3dbd276990c377f22f49fe2e53e37e01baabcb147ae4dd08.
The current combined build and staged module matched exactly; subsequent
source changes before installation were documentation and release tooling.
OBS was closed. All staged non-module files matched the existing
installation, so only the module was replaced atomically; the previous
module was backed up. Existing model files, runtime libraries, scenes,
and service configurations were preserved. RVC helper v3 and native
runtime checksums passed, and all three speech worker health endpoints
reported the options protocol.

Installed-module verification:
- Exact seven-effect registration passed in isolated libobs.
- Real YAMNet CPU inference and stereo preservation passed at 48 kHz.
- DDSP passed on retry at 48 kHz: 595 hops, 1.157 ms last worker hop,
  instrument morph/switch, pitch, noise, dry/wet and silence checks.
  First attempt remained loading beyond the short readiness window;
  cold-start timing remains unqualified.
- Native CUDA RVC passed at 48 kHz with the existing v3 worker and
  qualified synthetic speech fixture: 16 hops, 297.7 ms last hop,
  nonzero wet audio, mono wet and stereo dry checks passed.

Both final local CI preview archives passed the shared package auditor
after the formatter launcher correction. This installation does not
clear redistribution rights or establish uninterrupted combined
STT/TTS/stem/SA3 acceptance. No model or runtime binaries were added
to Git, and no services were restarted or OBS scenes changed.

## Native Plugins settings (2026-11-25)

Added optional SOUND_SETTINGS_PAGE Qt integration using the existing
Aftermarket sidebar/stack pattern; shared versioned tab convention lets
independent plugins contribute without linking each other. OBS 32.2.0
installed locale confirms its existing Plugin Manager Browse/Installed/
Updates; no duplicate manager. Sound adds model/runtime chooser roots and
compiled-effect resource links. No automatic downloads, cloud changes,
source migration or device override. RED missing shared bridge; GREEN
two-provider attach/detach, rejected Apply, successful Apply, Cancel,
layout mismatch; real libobs property default probe confirmed persisted
roots without changing selected model defaults. Isolated actual OBS
Fluffy Dark showed Plugins / LLL.sound, Browse, Apply wrote selected
directory, Cancel preserved it, reopening restored it, OK cleared it.
No normal OBS config edited. Other plugins not migrated, cross-version
and native plugin-manager behavior not tested. Qt integration remains
opt-in and baseline CI archive is unchanged in scope.

## Runtime resource package boundary (2026-11-26)

Install the manifest and locale explicitly rather than copying the entire data directory.
The preview CI stages a synthetic untracked canary and verifies the exact payload
allowlist; local assets must not enter the distributed package.


## Shared stereo stem mixer (2026-11-27)

The historical multi-stem source and filter remain quarantined. Reused
their workflow, not their mono inference, global output-slot ownership
or independently drifting queues. The current buffered filter accepts
a complete validated LLSTEM01 four-stem response into one 8-plane
bounded batch ring with a single publication and playback cursor. No
new model requests for selected stem, instrumental, custom level/mute/
solo changes. Existing setting and source ID remain compatible. Gain
snapshot is one lock-free Q12 word; changes apply at packet boundaries,
not smoothed; user must allow summing headroom. The queue reserves 24 s
of stereo for four stems (~35.2 MiB at 48kHz). Generation reset discards
the whole batch stream; no dry leak and 12 s prefill unchanged.

Model-free red/green tests for both rates, arbitrary packet sizes,
mixes, mute/solo, restart and malformed batch passed, including UBSan.
Protocol client receives all named planes in one request and rejects
nonfinite values even in a different stem. Existing cancellation,
stereo transport, generation and 12 Python tests/isolated registry
passed. New real-libobs mix test with deterministic TEST ONLY peer
checked exact instrumental/custom/solo/mute/restore energies, three
requests and zero retries. Actual CUDA resident FP32, shifts 0,
overlap .1, segment 4 with synthetic stereo fixture also passed
the same live settings sequence (windows 1->3, zero retries, mute
exact zero), auto reconnect and socket reload (one retry each,
three recovered windows, zero stale/prefill energy). Nine requests
processed .405-.442 s each; one model load across ten windows including
warmup. 10,700 actual-model instrumented filter callbacks: no observed
allocation, frees, locks, waits, or >10ms callbacks. Max wall 2.497ms
mostly off CPU; two ordinary OBS instances were running, not stopped.
Fixture callback test also passed (2,500 callbacks). This does not
establish a hard real-time guarantee, music quality or end-to-end
full expanded profile. Independent OBS outputs, integration routing,
cross-window overlap-add and VRAM governor are separate work. No
installation, push, or user scene changes.


## Independent shared stem sources (2026-11-27)

Restored the old Multi-Stem Source workflow as ordinary public OBS
input sources, `stem_live_output` (LLL.sound Stem Output), under the
same opt-in `stem_live` effect gate. The existing filter now supports
Separate outputs (silent parent). Each child stores filter UUID and
named stereo stem, works even if created before parent attachment,
and uses the regular OBS mixer/routing path. No global OBS output
slots are allocated, no automatic scene mutation or upstream source
activation. Parent custom mix controls affect combined output only.
The old source IDs and ONNX engine remain quarantined.

One capture/render cursor yields both the optional combined mix and
all eight raw planes. A bounded 64-slot, up-to-1024-frame packet ring
publishes one timestamped batch from the audio callback to a separate
dispatch worker. Worker-only registry with weak OBS source references
and immutable bindings; no registry locks, reference management,
OBS output calls or allocations on the parent callback. Child source
updates/destruction invalidate old bindings; attach/detach and explicit
enable transitions invalidate the parent generation. Queue overflow
faults the entire generation; stale queued packets emit silence if
dispatched while still attached. A packet already in dispatch may
finish during a concurrent control change. Child sources remain
independent of producer lifetime. Direct input timestamps; no
hardware-clock/discontinuity correction. The sidecar protocol and
four-second processing allowance / 12-second prefill unchanged.

Evidence:
- Red registry test confirmed old module lacked Stem Output.
- Queue wrap/full/empty and 20,000 concurrent complete packets passed;
  both-rate raw-plane + mix exact tests, recovery, malformed batches
  and existing stereo queue tests passed under UBSan. All 12 Python
  tests, client cancellation/contract, isolated module registry
  (two IDs under stem_live, old quarantined IDs absent) passed.
- Deterministic TEST ONLY peer: four normal sources each captured
  1,152,000 frames, identical timestamps and exact expected independent
  stereo energies; three requests, zero retries, parent exact silence.
- Actual local resident CUDA FP32, shifts 0, overlap .1, segment 4:
  real libobs scene with four sources, each 1,152,000 frames, exact
  packet timestamp equality, four distinct nonzero stereo energies,
  zero initial dry energy, parent muted, two completed requests,
  no retries; renaming producer did not break UUID binding.
- Final source revision (module SHA256
  e87a9bc96fe1c6a0e7739d55576df15a0741d4d9f13f89a0bc9709cb1ccbbf17):
  49 s actual CUDA fanout with socket removed from settings at 15 s
  and restored at 25 s. All four sources each 2,352,000 frames,
  identical timestamp sequences, fault and fresh prefill exactly zero,
  all recovered nonzero, parent zero throughout. 4,900 dispatched
  packets, zero drops, four subscribers, three completed inference
  windows, one automatic retry. Filter picker found real UUID;
  producer detached while children remained alive, no further child
  packets during one-second feed. Existing test-harness global slot
  and empty slots 2-5 unchanged. Model loads=1 including warmup,
  window times .407/.455/.439 s. 4,900 instrumented filter callbacks
  no observed allocations/frees/locks/waits or >10ms; wall p99 27.241us,
  max .620ms, CPU p99 18.935us. Earlier real normal fanout 2,400
  callbacks p99 26.048us and no observed violations. Fixture normal
  fanout 2,400 callbacks also passed. Instrumentation is the parent
  callback, not a guarantee about OBS internals or arbitrary subscribers.
- Aftermarket ObsRouting::list enumerates audio-capable non-filter
  sources by UUID; HyperShaderFX uses scene/unplaced audio-capable
  source picker by UUID and obs_source_add_audio_capture_callback.
  Normal Stem Output sources fit these interfaces without a new API.
  Source inspection only; actual combined plugin runtime is not tested.

Runner now includes raw plane/dispatch unit coverage, real mix and
fanout normal/recovery cases. Protocol fixture wrapper includes normal
fanout; full extended wrappers were not run end-to-end. Real OBS
fanout measured at 48 kHz only (44.1 kHz transport tested model-free).
Explicit enable toggle/rebind/scene reload/long soak, queue overrun
recovery in actual OBS, clock discontinuity, actual multitrack
recording and combined plugin runtime need further qualification.
No installation, push or changes to user scenes or running OBS
instances. Remaining roadmap: cross-window overlap-add, latency
reduction only after measurement, source lifecycle stress, shared
worker controls and VRAM governance.


## Stem v1 compatibility reference (2026-11-27)

Added docs/stem-api.md as the code-derived baseline for LLSTEM01 and
the two native OBS snapshot procedures, linked from README and the
expanded build guide. Exact 36-byte header/PCM plane layout, both rate
payload sizes, error and local socket trust rules, persisted settings,
source UUID bindings, timing and counter semantics are documented.
Fanout counters are not per-destination delivery acknowledgements;
the socket transfer cap is not the audio deadline. Preserve v1
geometry and ABI; an incompatible extension requires versioning.
No runtime behavior or default settings changed.

The Python suite now reads the documented golden request, success
and error bytes and checks the actual codec, rejects malformed header
fields, and exercises the real server handler on nonfinite PCM and
invalid headers (exact error header, no payload, no inference).
The C++ client regression checks independent golden request/success
bytes. Isolated model-free runner passed 14 Python tests, stereo
transport/recovery, shared mix, dispatch, cancellation and exact
two-type OBS registration. All 12 local reference links resolved.
No new real-model measurement or combined-plugin acceptance; prior
CUDA evidence remains unchanged. Next: evaluate overlap-add and
lower latency against the unchanged v1 interface.


## Multi-family separation candidate probe (2026-11-27)

Broadened roadmap beyond HTDemucs after inspecting the installed
audio-separator catalog and actual audio.cpp registry. Exact cached
Kijai Mel-Band FP16 SafeTensors hash verified; direct strict model
load and 1.4/2.8/8 s CUDA FP32 stereo probes passed twice.
Warm repeat medians .1520/.2948/.9361 s, peak allocated 1030–1705
MiB, outputs finite and nonzero, instrumental residual derived.
No live OBS or quality qualification; no dependency install.
See docs/stem-model-families.md and tests/verify-stem-melband-model.py.
Preserve LLSTEM01; qualify named two/six-stem geometry separately.
Native audio.cpp source supports separation but installed RVC/VAD
registry does not; isolated native build required. Next: short-context
quality/overlap comparison and native GPU candidate qualification.
