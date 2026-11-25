# Audio Diffusion (opt-in native source)

**LLL.sound Audio Diffusion** is a local music and sound generator, independent
of the quarantined MusicGen source. It uses the pinned native SA3 V1 C ABI
qualified in [sa3-native-qualification.md](sa3-native-qualification.md).

## Build and use

Select `diffusion` in SOUND_EFFECTS with SOUND_EXPERIMENTAL=ON. For the
expanded local build the exact set is `mic;stem_live;yamnet;tts;stt;diffusion`.
Pass SOUND_SA3_RUNTIME_DIR pointing to the qualified libsa3 staging directory
including the matched V1 header, private GGML shared objects and licenses.
CMake rejects missing required files and mismatched ABI headers. The runtime
is installed alongside the module under sa3-runtime. The default quarantined
build does not register this source or link SA3.

Add an Audio Diffusion source to an active scene. Set the model directory to
the folder containing the verified GGUF files (see model manifests in
tests/fixtures). Small Music and Small SFX use Q4_K_M DiT, F16 autoencoder,
F16 text encoder. Select a family explicitly. The two families share the
T5Gemma files. No model is bundled or automatically downloaded. Auto prefers
GPU where available; CPU is an explicit option. The backend log identifies
the actual selected compute device.

### Playback and audio-to-audio

- Generate and play: prompt, optional negative prompt, 1–60 s, 1–32 steps,
  seed (-1=random). Replay uses the last successful output without inference.
- Start continuous music: Small Music only, 6–56 s segments. Generate the
  first segment, then extend from the last four seconds while the current
  segment plays. Retain 350 ms until the next segment is ready; the native
  splice, plus a short host-side boundary blend, prevents a discontinuity
  from normalizing the unplayed prefix. All segments share one sample clock.
  Change prompt, negative prompt, steps, duration, seed and conditioning for
  subsequent segment requests. The current model/device is fixed until restart.
  If an extension fails or misses its deadline, play the retained tail and
  stop with an error rather than hide the failure by looping or fabricating
  silence. A long session is not an ever-growing in-memory waveform.
- Transform / extend: select an OBS audio input (1–30 s capture), a local
  PCM16 mono/stereo WAV (44.1 or 48 kHz), or leave both blank to use the last
  clip. A selected source takes precedence over a WAV. Capture is explicit,
  bounded, and rejects incomplete/non-finite audio. The original input is
  unchanged; the generated result is a separate OBS audio source, not a
  zero-latency inline effect. Capturing a muted source requires opt-in.
  Transform strength is 0–1 and keeps input duration. Extend adds the
  selected duration, with a 60 s total result cap.
- Finish current segment stops continuous generation after the current
  segment and its retained tail, without starting another clip. Pending
  lookahead is cooperatively cancelled or discarded. This is a segment
  boundary, not a beat-aware ending. Stop cancels immediately.
  Replay cache is session-local;
  in continuous mode it holds the last generated segment, not the entire
  soundtrack.

### Automation and reactive conditioning

The optional advanced group accepts a second prompt that ramps from 0 to
full weight across the new segment using the native SA3 lane ABI. Two
compatible local GGUF LoRA paths may be applied in order with individual
0–2 strengths. LoRA B may instead fade in on the same native lane; its
metadata must advertise lane compatibility. Select adapters intended for
the chosen base family; the V1 metadata API cannot fully certify base-model
compatibility. No adapters are downloaded or bundled. The acceptance test
uses a nontrained rank-2 adapter fixture built against a real DiT tensor;
it tests the host path, not trained-adapter quality.

Sound context optionally consumes a fresh final STT result or YAMNet
classification (score at least 0.4). Sight motion optionally consumes the
versioned U2Net motion snapshot and maps normalized energy to calm, gentle
or energetic musical conditioning. Both are sampled when scheduling each
generation, before model loading/queue delays; missing or stale cues leave
the base prompt unchanged. These are local prompt-conditioning cues, not
a separate language model, and recognized speech may appear in the native
runtime's local prompt log. Cue counters record sampled inputs, including
requests that subsequently fail.

### Execution and lifecycle

Each source has one control/playout worker and one pending command.
Continuous mode creates at most one lookahead generation task at a time;
all native diffusion calls serialize across Sound instances. Frugal model
residency and context destruction keep inference allocation separate from
the playout timeline. No inference or allocation occurs in the live capture
callback, and inference never runs on the OBS render/audio callback. New
commands supersede previous epochs. Disable, deactivate and destruction
cancel outstanding work; native cancellation is cooperative.

PCM is validated for exact output format, finite samples and bounded
duration. A uniform peak ceiling of 0.75 leaves resampling headroom;
this is not a mathematical guarantee against intersample peaks for all
possible waveforms. Playback submits sample-clocked 20 ms blocks.
Version 1 status remains unchanged; version 2 adds captured, automated,
sound_cues and sight_cues counters. The version 3 procedure
`sound_diffusion_status_v3` includes those fields and adds continuous,
finishing, generating, next_ready, stage, generation_ms (last successful
request wall time including queue/model loading), buffer_ms (current clip
remaining), next_ms (ready lookahead clip including its held splice),
played_ms (frames submitted in the current command) and underruns
(current command). Current and next clip durations overlap by the retained
splice, so they should not be blindly summed. Session measurements reset
when a new command is accepted; stop preserves the completed session's
elapsed and timing measurements. The phase and lookahead state clear
when the worker finishes. Counters in V1/V2 remain lifetime counters.
Refresh status displays this information; it is manual, not a live dock.
Runtime libraries use private GGML SONAMEs with local binding.

## Testing

Run `tests/verify-sa3-native.sh` for direct runtime qualification.
Set SOUND_SA3_STAGE and SOUND_SA3_MODELS and run
`tests/verify-sa3-obs.sh` for the actual isolated OBS source at 48 and 44.1 kHz.
The tests exercise generation, non-silent stereo capture, replay without
inference, cancellation after reported sampling progress, invalid-model
recovery and disabled-source rejection. The existing STT OBS test programs
also support optional SOUND_TEST_SA3_MODELS to run actual generation alongside
their ordinary transcription tests. This does not certify every third-party
plugin or every combination of GPU workloads.

## Additional acceptance

Use `tests/verify-sa3-upgrades.sh` with SOUND_SA3_STAGE,
SOUND_SA3_SOURCE, SOUND_SA3_MODELS and SOUND_SA3_INPUT_WAV. It checks
both model manifests and the runtime checksum, constructs a temporary
test-only adapter, runs real SFX, WAV/source transformation, extension,
native adapter/prompt automation and Sight ABI fresh/stale fixtures,
continuous playback at both rates (including graceful finish and status
readback), and the original manual regression suite. Set SOUND_SA3_SOAK=1
when running the continuous harness directly for ten transitions instead
of three; use a timeout of at least 100 seconds. For actual STT conditioning, set SOUND_TEST_SA3_ADAPT=1 and
SOUND_TEST_SA3_MODELS with the existing real STT harness. It waits for a
recognized final phrase before starting diffusion.

No claim of perceptually seamless music for every prompt, indefinite
soak stability, arbitrary third-party LoRA compatibility, live camera
qualification, every Sound/Sight signal source, or full expanded
acceptance is made. Audio capture and transformation use this source's
separate output; a continuously processing inline audio-to-audio filter
and a dedicated generative soundboard/dock remain distinct work.
