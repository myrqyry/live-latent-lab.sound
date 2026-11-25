# Audio Diffusion (opt-in native source)

**LLL.sound Audio Diffusion** is a manual local music generator, independent
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
the folder containing all five verified Small-Music GGUF files (model manifest
in tests/fixtures). No model is bundled or downloaded automatically.
Choose Auto (GPU preferred) or CPU. Auto may fall back to CPU; consult the
actual backend log. Set the prompt, optional negative prompt, duration (1–60 s),
steps (1–32), and seed (-1=random). Generate and play runs on demand, Replay
reuses the last successful clip without inference, and Stop cancels pending
or running work. Refresh status reads the current stage/progress; it is not
an automatically updating dock. Cached audio is session-local, not saved
in the scene. Editing settings applies to the next requested generation.

Each source has a worker and one pending command. A new command supersedes
the previous one. Disable/deactivate/destruction cancels outstanding work;
a cooperative cancellation cannot interrupt a native call already in
progress until the runtime checks its flag. All inference is off the OBS
render/audio threads. Diffusion jobs serialize globally; each job uses
frugal model residency and destroys its model context before paced stereo
playout. It validates finite, bounded PCM, exact sample count and format,
then submits 20 ms blocks with monotonic sample-clock timestamps.
Memory is bounded by the 60-second clip limit and last successful clip.
The Linux runtime uses private GGML SONAMEs and RTLD_LOCAL/DEEPBIND/NODELETE;
no global library path or worker service changes are needed.

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

## Deliberate limits

This first source only exposes the qualified Small-Music Q4_K_M pipeline.
Small-SFX, continuous/auto continuation, input-audio transformation in OBS,
LoRA controls, reusable model catalog, and a live dock are not enabled yet.
No real-time DSP filter or zero-latency generation claim is made.
