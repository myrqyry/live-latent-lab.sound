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
