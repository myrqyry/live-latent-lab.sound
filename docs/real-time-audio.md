# Real-time audio transport checkpoint

## Callback contract

Capture/render/filter callbacks must not grow or free heap containers, take a
plugin blocking mutex, load models, access files, or run neural inference.
Preallocated vector storage is allowed. Each SPSC queue has a single writer
and a single reader; configuration and lifetime transitions are control-path
operations. A full queue rejects new data rather than overwriting unread data.

## Verified changes (Linux Mint, local libobs 32.0.1)

- Legacy Whisper: capture has no plugin mutex or locking OBS event signal.
  Callback removal synchronizes with dispatch before state destruction.
  Worker polling adds up to one 20 ms polling interval; the event wakes shutdown.
- Shared mono/stereo queues: contiguous wraparound copies, no heap activity.
  Three paired 1024-sample write/read microbenchmarks on Ryzen 7 7700:
  median mono 1148.43 -> 90.84 ns; stereo 2986.03 -> 205.33 ns.
  These are isolated queue measurements, not total OBS audio latency.
- Matchering: planar analysis and in-place DSP, a lock-free coherent reference
  and settings snapshot, WAV loading only on the control path. Old/new engine
  comparison gave zero sample error on the tested mono-mix stereo fixture,
  two sample rates, four block sizes and three mix amounts. Real OBS audio
  test exercised dry/wet/reference-clear transitions.
- Legacy Demucs source: bounded 30-second capture; discard queued input on
  discontinuities; reusable worker chunk; lifetime-safe private child output
  binding and one read per rendered block for all requested OBS mixers.
  Child source creation must follow parent registration; an independently
  created orphan child remains silent instead of rebinding on the audio path.
- Legacy Demucs filter: three preallocated output blocks (up to 10 seconds
  per block, four planar stems), tagged by configuration generation and mode.
  Reject full output queues and stale batches; underrun renders silence.
  Standard model configuration changes and clearing a reference invalidate
  previous output. Single-stem delayed dry fallback is retained.

## Repeatable tests

Model-free queue and DSP checks are included in the public CI verification
script. Additional local checks:

    bash tests/verify-matchering-obs.sh
    bash tests/verify-demucs-source-rt-obs.sh /path/to/onnxruntime-src
    bash tests/verify-demucs-filter-rt-obs.sh /path/to/build/_deps
    bash tests/verify-demucs-no-fake-stems.sh
    bash tests/verify-quarantine.sh

The Demucs direct callback tests use synthetic separated samples and real OBS
packet/mix structures. They prove transport and routing, not model inference.
The legacy Whisper test is a source-level guard plus local syntax verification,
not a live teardown stress test. The C++ heap guards cover ordinary new/delete
in the tested paths, not arbitrary malloc or all third-party operations.

## Remaining qualification

- Actual legacy Whisper and Demucs model inference, reload/teardown stress,
  sustained throughput and end-to-end latency; no new effect registration
  gates or installed user plugin changes.
- Demucs multi-source stem queues remain independently bounded: if one child
  does not consume, its batch drops can differ from the other children.
  Advanced Demucs single-stem model selection remains a separate correctness
  issue; transport changes do not qualify every engine output.
- DDSP/RVC per-sample wet-queue reads, STT packet copying and wake coalescing,
  DeepFilter overflow handling, and third-party allocator/lock instrumentation
  remain optimization/verification candidates.
- Public hosted CI and other platforms have not been run for this checkpoint.
