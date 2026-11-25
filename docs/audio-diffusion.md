# Audio Diffusion (opt-in native source)

**LLL.sound Audio Diffusion** is a local music and sound generator, independent
of the quarantined MusicGen source. It uses the pinned native SA3 V1 C ABI
qualified in [sa3-native-qualification.md](sa3-native-qualification.md).

## Build and use

Select `diffusion` in SOUND_EFFECTS with SOUND_EXPERIMENTAL=ON. For the
expanded local build the exact set is `stem_live;yamnet;tts;stt;diffusion`.
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

### Native performance dock

Enable `-DSOUND_DIFFUSION_DOCK=ON` with the diffusion effect and matching
OBS frontend API / Qt 6 Widgets development packages. This is independent
of the historical Tools menu and defaults off for headless builds.
Open **LLL.sound Audio Diffusion** from OBS Docks. Select an existing
Audio Diffusion source by UUID; no additional source is created. The
dock displays real V3 generation progress, playback, lookahead and
underrun status every 500 ms while visible. Generate, Continuous, Finish
segment, Stop and Replay call the same source commands as the properties
dialog. Prompt, duration and steps use Apply/Revert; a dirty draft
blocks source switching and new generation until explicitly applied or
reverted. An external edit to a drafted setting is rejected rather than
overwritten. If the source disappears, the dock drops the draft and
disables the controls. The model directory can be selected directly.
Use source properties for model family, device, WAV/source capture,
LoRA and reactive conditioning. The dock follows the OBS theme.
It does not start generation on load or automatically select a source.

Run `tests/verify-sa3-dock.sh` with SOUND_SA3_STAGE and SOUND_SA3_MODELS
for Qt/libobs verification of source discovery, selection, draft/apply
isolation, real CUDA generation, live telemetry and source removal.
Actual OBS startup additionally qualifies frontend dock registration;
the Qt harness is not a screenshot or full manual frontend interaction test.

### Adaptive playback reserve

Enable **Adaptive reserve (longer startup)** in the dock or
**Adaptive playback reserve** in source properties before starting
continuous music. The existing single-lookahead mode remains the default.
Adaptive mode generates the first segment and pre-fills two continuations
before starting playback. It then keeps a bounded queue of two to three
ready clips using recent generation wall time (including model loading
and inference-gate contention). The policy keeps a one-second margin,
raises the target when generation approaches segment duration, and decays
its high-water estimate as generation speeds recover. It does not change
the selected diffusion steps or prompt. Only one producer generates the
next clip, always extending the last generated clip in chronological order.

Reserve duration excludes the held 350 ms splice region of each queued
clip; V3 next_ms still describes the first queued clip. The separate
`sound_diffusion_buffer_v1` status procedure reports adaptive,
prefilling, queued_clips, target_clips, reserve_ms and prefill_ms
(the generated reserve at playback start). The dock updates these fields
while visible. The mode applies on the next continuous start, and pending
prompt changes apply to future generation rather than already buffered
audio: with six-second segments, expect roughly 12–18 seconds of queued
audio in addition to the current segment. This is a reliability/latency
trade-off, not a zero-latency live prompt mode.

Stop invalidates the current command and queued clips; Finish segment
discards queued future clips, cancels pending generation and plays only
the current clip's remaining tail. If the reserve runs out, playback ends
at the available tail and reports an underrun, without fabricating audio.
A queue cap of three keeps memory bounded independently of session length.
The reserve cannot compensate indefinitely for generation slower than
playback, nor guarantee coexistence with arbitrary GPU workloads.

Run `tests/verify-sa3-buffer.sh` with SOUND_SA3_STAGE and
SOUND_SA3_MODELS. It checks the deterministic policy and actual CUDA
continuous playback at both rates, including a competing 60-second
32-step generation at 44.1 kHz. The latter is a specific same-plugin
contention test, not a comprehensive GPU-load qualification.

### Neural latent morph

Use the optional `-DSOUND_SAME_SOURCE_DIR=/path/to/pinned/sa3.cpp`
with the existing qualified CUDA/private GGML runtime. This builds a small
native helper, `sound-sa3-morph`, next to libsa3 in sa3-runtime. The build
checks the sa3.cpp and GGML revisions and tracked cleanliness. The helper
checks the installed Small-Music SAME-S F16 model size and SHA256, uses
only the autoencoder (no T5Gemma or DiT), and reuses existing model weights.
It loads no new inference runtime and requires no Python. Upstream SAME
implementation comes from the MIT-licensed pinned sa3.cpp headers.
Model license remains separate. No ONNX model was imported.

In Audio Diffusion source properties, open **Neural latent morph / texture (SAME-S)**.
Choose two stereo PCM16 WAVs at 44.1 kHz of identical length (8192 samples
through 12 seconds). Choose Latent blend 0–1 or enable the ramp from A
toward the selected blend. Click Process and play. The helper encodes
both signals into SAME's 256-dimensional latent sequence, interpolates
those features and decodes the result. The output has the original
duration, bounded amplitude and exact stereo format. Fixed blend 0 or 1
still passes through neural reconstruction; it is not bit-perfect bypass.
The ramp changes the blend over latent frames, not PCM gain.

The Operation selector also offers three single-input texture effects.
For these, choose only Audio A; Audio B and the A/B ramp are ignored.
- Freeze repeats the latent frame selected by position across the sequence.
- Temporal smear averages neighboring latent frames using the selected
  radius (1–16 frames on each side, truncated at the clip boundaries).
- Latent grain repeat cycles a contiguous 1–16-frame region starting
  near the selected position, clamped to fit the available sequence.

Blend / effect amount interpolates between the original and modified
latent sequence. Zero reconstructs the unmodified input through SAME;
it is not a bit-perfect dry bypass. Each latent frame represents 4096
samples (about 93 ms). The operations exclude completely padded frames
from anchor/grain selection. Freeze and grain are neural texture
operations, not spectral freeze or a conventional PCM granular engine.
All retain the input clip duration and use the existing replay/cancel
path. For longer grains than the available input, the length is clamped.
The clip buttons process files; the separate live texture command below
processes a selected live source.

The current family/step/negative prompt settings do not affect morphing.
Auto prefers CUDA; CPU can be selected explicitly. No sample format
conversion for file morph, automatic truncation of different input durations,
live two-input morph, beat alignment, or per-frame slider scrubbing is provided.

The OBS worker launches a fixed local executable with argv (no shell)
and shares the existing inference gate with diffusion. A/B morph and
one-shot fallback use a private temporary output directory; resident
texture requests use the socket transport described below. It waits for the result off the audio/UI
threads; cancellation terminates and reaps only its own helper process.
A 45-second deadline bounds each helper request. Successful results use the
existing playout and replay cache. Existing inputs remain unchanged.
A helper crash or validation failure cannot directly abort OBS.
This is batch generation, not a low-latency inline audio effect.

The executable can also be used directly:
`sound-sa3-morph MODEL A.wav B.wav 0.5 output.wav auto fixed`
or use `ramp` and an explicit `cpu` or `gpu:CUDA0` device.
For single-input textures:
`sound-sa3-morph MODEL A.wav "" 0.8 output.wav auto grain 0.4 3 4`.
The last three arguments are normalized position, smear radius and
grain length. Select freeze, smear or grain as the operation. The standalone
command refuses to overwrite an output file.

Run `tests/verify-sa3-morph.sh` with SOUND_SA3_STAGE,
SOUND_SA3_MODELS, SOUND_SAME_INPUT_A and SOUND_SAME_INPUT_B (two
different two-second WAVs). It measures actual endpoint reconstruction,
GPU/CPU waveform agreement, distinction from PCM crossfade, ramp output,
safe invalid-input behavior, real OBS playback/replay and cancellation
recovery. Run `tests/verify-same-texture.sh` with the same environment for
deterministic latent indexing, effect amount zero, distinct audio for
all three modes, CPU/GPU agreement and actual OBS playback of each
mode without a second WAV. Subjective quality and live neural granular
processing quality remain separate qualification steps.

### Resident SAME-S texture worker

Single-input freeze, smear and grain requests now use a resident
autoencoder helper rather than launching a new process for each clip.
The first request verifies and loads the same pinned model. Subsequent
requests reuse the model, encoder/decoder graph pair and bounded
host buffers. A different padded frame count replaces the graph pair;
there is no unbounded graph cache. PCM is transferred directly through
a private local socket, not temporary WAV files.

One warm context is allowed across Sound sources. If another source
already owns it, the existing isolated one-shot path is used. Batch inference still shares the diffusion gate; idle warm weights can remain
resident while another source runs diffusion. The live single-input codec
is a bounded concurrent exception described below. The worker is released
after eight seconds of inactivity following playback, on Stop,
deactivation, destruction, or before another kind of generation in
the same source. Model/device changes are picked up on the next request.
Model verification happens once per worker lifetime. A/B morph remains
on the original isolated batch path.

The source status procedure `sound_same_resident_v1` exposes lifetime
worker starts and successful resident requests, the current worker's
graph-pair build count, and active (0/1). The Properties status text
shows warm state and counters. The worker can be cancelled during
model loading or inference without blocking the audio callback.
The private little-endian socket protocol uses bounded frame counts
and sequence-tagged replies; the worker has no listener or network
service. Invalid parameter/sample requests are rejected; framing,
backend or connection failures terminate the worker and the source
reports failure rather than playing stale audio.

For direct qualification, invoke
`python3 tests/verify-same-resident.py HELPER MODEL A.wav gpu:CUDA0`
or select cpu. Set SAME_RESIDENT_SOAK=1 for the additional 60 changing
16384-frame requests. The harness checks reuse, changing inputs,
geometry replacement, invalid requests, clean EOF and measured
short-block throughput. Use SOUND_SAME_RESIDENT_OBS=1 with the existing
texture runner to exercise warm reuse, Stop, restart, idle expiry and
resident cancellation in actual OBS. Short-block throughput is not a
measurement of live audio latency, decoder seam quality or an inline
filter. Live scheduling and overlap/context handling are described below.

### Continuous live neural textures

Add a separate Audio Diffusion source to the active scene. Select an
existing audio input using **Capture audio source (optional)**, then
choose Freeze, Temporal smear or Latent grain repeat in the
**Neural latent morph / texture (SAME-S)** group. Choose Live reserve
(3–12 blocks) and click **Start live texture**. Stop / cancel ends the
session. The dry source remains untouched; the generated output is a
separate delayed wet channel. Mute or route the dry channel separately
if you only want the wet output. Captured muted packets are replaced
with zeros unless Capture muted source is explicitly enabled.
A/B morph continues to be a two-file batch operation.

At the model's 44.1 kHz rate, the resident codec processes windows of
32768 samples (743 ms), advancing 16384 samples (372 ms) per request.
48 kHz input is resampled using libobs on the inference producer,
not the audio callback. The previous decoded second half and the next
decoded first half are blended linearly at matching sample positions.
The first block fades in over 20 ms. This preserves the sample count,
reduces boundary discontinuities and does not resample each window
independently. Output uses one sample-count clock for the session;
no per-window clock reset. Changes to amount, position, radius,
grain size and single-input effect apply to newly processed windows,
not previously buffered audio. Input source, compute device and reserve
apply on restart. Freeze/grain selections are per-window positions,
not a persistent global freeze latch.

Four ready blocks is the default; the observed startup delay was
about 1.86 seconds. Eight blocks measured about 3.35 seconds. Reserve
trades latency for scheduling headroom. The input ring, ready queue
and graph cache are bounded. A full ring, invalid sample/timestamp
discontinuity, stalled input, removed source, native worker failure
or exhausted reserve stops wet playback and reports an error; it never
silently drops time or replaces it with fabricated audio. Restart
explicitly to recover. The callback copies input into preallocated
storage; inference, resampling and output pacing run off that thread.

One live resident codec may run in its own process concurrently with
the serialized batch diffusion gate. This avoids starving the wet
channel behind a multi-second diffusion job. The residency slot
prevents additional concurrent warm codecs; another live source must
wait until the existing one is stopped. Concurrent resource exhaustion
can still stop the wet source. Actual CUDA contention with a separate
60-second, 32-step SA3 generation passed; arbitrary GPU saturation is
not guaranteed. A/B morph, one-shot fallback and batch generation
retain their existing serialization rules. The procedure
`sound_same_live_v1` exposes running, processed blocks, dropped input
frames (final count on stop), and measured startup delay_ms. Existing
diffusion status V3 exposes playback and underruns.

Use `tests/verify-same-live.sh` with SOUND_SA3_STAGE and SOUND_SA3_MODELS
for overlap math, actual 48 kHz default reserve, 44.1 kHz competing
generation, 40-second 48 kHz soak and stall/restart. The actual
fixtures checked finite bounded output, monotonic near-contiguous
timestamps, dry continuity and measured sample discontinuities.
This is a delayed live wet source, not a zero-latency inline filter.
Subjective sound quality, long-duration hardware clock drift and
arbitrary GPU workload coverage are not established by these tests.

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
Refresh status displays this information in source properties; the optional
performance dock updates automatically while visible.
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
and a dedicated generative soundboard remain distinct work.

### Advanced generation model settings (2026-11-25)

The source properties now expose CFG (0–20, default 1), distribution
schedule (LogSNR, Flux, Full, None; default LogSNR), generation tail
padding and continuation tail padding (0–12 seconds, both default 6).
These are forwarded through the actual libsa3 V1 request and sampled
at the next continuous-segment boundary. Defaults preserve the old
behavior. CFG other than 1 adds an unconditioned inference pass;
higher guidance or padding can change throughput and GPU memory.
For the currently used SA3 distilled variants the SAT-only sampler and
sigma fields have no effect and are not presented as editable controls.
The dock retains its smaller quick-control view; these advanced
settings are in source properties.

Real libobs Small Music tests with CFG 1.5, None distribution,
3-second generation tail and 4-second continuation tail passed
48/44.1 kHz generation (95983/88200 frames), replay, cancellation,
missing-model failure, recovery and disable. The first lifecycle run
timed out; a focused single generation completed in 3874 ms including
playback, and the full lifecycle retry passed at both rates. This
does not qualify all numeric extremes, SFX variants, or continuous
playback at all new settings. See the model-settings audit.
