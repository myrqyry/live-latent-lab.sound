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
