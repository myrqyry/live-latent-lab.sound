> Historical qualification before microphone DSP moved to HyperDSP. The current Sound foundation harness tests only neural filters.

# Sound: verified audio foundation (2026-10-13)

This is a deliberately narrow verification checkpoint for lll.sound.
A successful CMake build is **not** evidence that an AI effect works with its
model or even that OBS can load the plugin.

## Effect quarantine since this checkpoint

All effect registrations are now disabled by default. Run
tests/verify-quarantine.sh for that new, dependency-light baseline. The
older suite below remains useful only with a deliberately configured
experimental effect build (such as mic), not the normal zero-effect build.
See docs/effect-quarantine.md for the exact registration gates.

## Reproduce on Linux Mint

```sh
# After CMake has configured build_x86_64 and fetched its pinned dependencies:
./tests/verify-foundations.sh /path/to/experimental-build
```

This compiles actual source and uses real ONNX Runtime / libobs, not fake
engine classes. It creates a disposable package and loads it into an isolated
OBS context; it does not touch user scenes, system plugin directories or the
existing installed plugin binary.

## Evidence

- **Ring buffer:** SPSC ownership invariant preserved. Producer only writes
  head, consumer only writes tail; when full, new audio is dropped rather than
  overwriting samples currently being copied. 250,000 sequential samples pass
  concurrent FIFO verification, alongside overflow, wrap and reset tests.
- **Microphone DSP:** default bypass is bit-identical. Condenser high-frequency
  gain measured 1.217x at 9 kHz; Ribbon high-frequency gain 0.752x; Tape
  nonlinear difference measured 0.147 mean absolute sample deviation.
  Independent channel engines do not leak impulse/history between channels.
  OBS callback processes planar channels separately. UI/model reconfiguration
  uses a nonblocking try-lock fallback to dry audio.
- **No-model audio safety:** actual libobs callbacks preserve both channels
  unchanged when RVC or DeepFilterNet models are not configured; custom Mic
  ONNX does not report ready without a model.
- **Packaging:** Linux bundle installs ONNX Runtime, Whisper and GGML
  dependencies beside the module. Their RUNPATH is `$ORIGIN`; a staged module
  resolves these dependencies with no `LD_LIBRARY_PATH`, and `obs_open_module`
  + `obs_init_module` successfully register microphone and Whisper sources.
- **YAMNet disabled:** the discovered local LiteRT C library was a zero-byte
  file. Its source was absent from the CMake target while plugin-main still
  referenced its symbol, causing an undefined-symbol failure at runtime.
  The dormant source also performs classification in an audio callback and
  does not provide verified visual output. Do not register it until these
  contracts are implemented and checked.

## Not proven / remaining work

- None of the actual **model inference** paths has passed end-to-end
  verification here: RVC voice conversion (voice + pitch + content models),
  DeepFilterNet, Whisper, Kokoro, Chatterbox, Demucs, MusicGen, MediaPipe,
  Matchering or custom Mic ONNX. Merely constructing/loading ONNX sessions
  is not proof of correct tensor contracts, generation, or audible output.
- RVC / DeepFilter / neural Mic audio alignment under sustained throughput,
  sample-rate/channel adaptation, backend choices, underrun and queue overflow
  policies remain unverified. The custom Mic model currently processes channel
  0 only; other channels remain dry, not cloned.
- Models loaded synchronously through settings callbacks and their state
  transitions still need asynchronous swap and clear failure/latency UI.
- The broader advertised suite needs separate authentic model + audio
  fixtures, latency/performance measurements and isolated OBS integration
  before any individual effect is marked working.
- The isolated loader has no OBS frontend UI; its Tools-menu callback warnings
  are expected in that test and **do not** verify the actual GUI/dock.
- A *local user OBS installation* and GPU inference have **not** been tested
  as part of this checkpoint. Existing installed binaries were left alone.

## Next targeted milestone

Start with **RVC or Whisper** using a known-compatible actual model and
captured test audio. Record exact model I/O and sample-rate/timing behavior.
Test a successful transformed/transcribed result and the no-model/error bypass
as independent assertions before advertising the feature as live.

### STT pre-roll storage

The unified STT filter retains the latest 200 ms (3,200 samples at its
16 kHz internal rate) of pre-speech audio in a worker-owned fixed-capacity
circular buffer. Appending an audio frame uses at most two contiguous copies,
without allocating or advancing a deque one sample at a time. At speech
onset the chronological window is copied into the existing phrase vector;
stream reset discards its logical contents. Audio callback ownership,
silence thresholds, resampling and utterance boundaries are unchanged.

The capture regression compares the buffer against the previous deque
behavior across 200 variable-sized packets, wraparound, resets and oversized
packets. Capture, wake, caption history, configuration and SPSC regressions
pass. A fresh STT-only staged module passed actual libobs Whisper fixture
checks at 48 and 44.1 kHz, including dry output, transcription, mute, stopped
source flush, hot settings, and lifecycle recovery. An initial cold model
load took 15.7 seconds and missed the existing 10-second readiness deadline;
the warmed runs reached ready in 200 ms. No change to model loading or
startup deadline, no measured CPU saving, no other backend or full
combined-package qualification. Evidence: /tmp/sound-preroll.U1poYB.
