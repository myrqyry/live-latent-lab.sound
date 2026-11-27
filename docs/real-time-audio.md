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

## Second pass (2026-11-27)

- DDSP and native RVC read wet samples into bounded 256-sample stack
  blocks, eliminating the per-sample queue read and cursor publication.
  Startup delay, mix ramp and delayed-dry fallback are preserved.
  An underrun consumes the available prefix and latches failure; a worker
  fault is observed at block boundaries (at most 256 samples already read).
  Local Ryzen 7 7700 microbenchmark, median of three paired runs for
  480-sample packets: DDSP 2621.0 -> 2517.4 ns; RVC 3141.6 -> 2694.7 ns.
  These include simulated queue service, not neural inference. Output
  checksums matched the old implementation.
- STT writes downmixed samples directly into unpublished preallocated queue
  slots, copies only the valid prefix on consumption, reuses the worker's
  packet and coalesces notification per OBS packet. Epoch and timestamp
  semantics are retained; a full slot does not invoke the writer.
- DeepFilter uses a generation handshake between audio and worker actors.
  Overflow and model changes invalidate the stream. Each queue's consumer
  discards the old backlog; neither actor resets the other actor's cursor.
  The worker resets recurrent state before acknowledging the new generation.
  While recovering, the callback uses the existing dry/silence preference.
  Model clearing or failed initialization always returns dry. Invalid-sized
  or nonfinite inference output requests recovery instead of being queued.
  Model initialization remains synchronous on the control path, not audio.

### Additional local evidence

- Combined seven-effect module (stem_live, yamnet, tts, stt, diffusion,
  ddsp, rvc) built and staged against local libobs 32.0.1.
- DDSP full native-model and OBS tests: 44.1 and 48 kHz passed, including
  missing-model recovery. Local LiteRT used its CPU delegate.
- Native RVC real OBS/model test: 44.1 and 48 kHz passed, 16 processed
  hops each; existing local helper v3 used.
- Unified STT real OBS/Whisper CPU test at 48 kHz: 1,296,000 captured
  frames; expected speech recognized, caption routing, unchanged dry audio,
  muted input handling, hot update, stopped-source flush, disable and
  missing-model recovery all passed; zero reported dropped samples.
- DeepFilter: 200 concurrent transport generation changes passed. Separate
  real libobs/ONNX Runtime integration test used synthetic gain graphs
  (.25 and .75), verified wet output, model replacement and clearing.
  This is transport qualification, not actual DeepFilterNet noise reduction.
- All 12 focused C++ model-free checks, legacy Whisper source guard,
  public URL tests, Demucs no-fake-stems guard and quarantine build passed.

## Third pass: worker reuse and real dispatch timing (2026-11-27)

- DDSP analysis sanitizes/downmixes each input sample once into a reusable
  buffer, including the 48-sample FIR history. Exact output comparison
  against the prior implementation passed at both rates, with clipping,
  NaN/Inf and multiple hops. Isolated analysis timings: 44.1 kHz
  32.827 -> 7.553 us/hop; 48 kHz 29.428 -> 7.610 us/hop.
- Native RVC reuses the validated helper response and SOLA hop buffer.
  SOLA precomputes its fade coefficients; its alignment search and tie
  break are unchanged. Exact old/new comparisons and deterministic
  shifted-window tests passed. The reuse test guards ordinary C++
  new/delete after construction. SOLA's search remains the dominant
  CPU cost of stitching; measured changes were modest
  (48 kHz 1534.526 -> 1506.042 us per hop).
- Test-only dispatch recorder measures synchronous obs_source_output_audio
  including libobs and fixture capture, not individual callback duration.
  Fixed storage avoids heap growth in the measurement loop. Existing
  DDSP and RVC real-model tests print p50/p95/p99/max and packet-budget
  exceedances. No production telemetry or pass threshold was added.

### Concurrent real model dispatch measurements

Ryzen 7 7700, RTX 2070 SUPER, Linux Mint, separate isolated libobs test
processes using real CPU DDSP and CUDA RVC. System load was already high
(around 19-21 one-minute load average, not controlled); GPU utilization
was sampled once per second (83% peak before, 75% peak after at 48 kHz).
The 10 ms packet budget is not a hard real-time guarantee.

| Rate | Version | Effect | Calls | p50 us | p95 us | p99 us | max us | >=10 ms |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 48 kHz | Before | DDSP | 1190 | 10.168 | 18.204 | 31.017 | 849.819 | 0 |
| 48 kHz | Before | RVC | 900 | 10.269 | 17.212 | 24.716 | 61.913 | 0 |
| 48 kHz | After | DDSP | 1190 | 10.630 | 17.652 | 26.609 | 1119.804 | 0 |
| 48 kHz | After | RVC | 900 | 10.379 | 16.710 | 23.864 | 801.119 | 0 |
| 44.1 kHz | After | DDSP | 1190 | 8.816 | 16.831 | 25.176 | 43.230 | 0 |
| 44.1 kHz | After | RVC | 900 | 8.827 | 13.875 | 21.669 | 28.081 | 0 |

All six real-model runs passed their existing functional checks.
The maximum 48 kHz dispatch time increased in the after run; the data
does not establish a reduction in worst-case latency. Test processes
overlap but are not a same-scene OBS filter-chain benchmark.
The combined seven-effect module rebuilt and staged locally. All 13
focused C++ checks, SOLA deterministic tests, legacy Whisper source
guard, Demucs no-fake-stems guard and quarantine build passed.

## Fourth pass: isolated callback measurement in a shared OBS chain

Linux/glibc test-only instrumentation wraps registered production
filter_audio callbacks and intercepts malloc/calloc/realloc/free,
aligned_alloc/posix_memalign, pthread mutex and read-write lock acquisition,
and condition-variable waits on the callback's calling thread.
The shared-library positive control detected five allocation calls, four
frees, three locks and one wait. Hidden libc symbols, custom allocators,
other synchronization APIs, kernel internal locks and other threads are
not covered. No instrumented code ships in the plugin.

Two 36-second tests used the same staged seven-effect build, a 48 kHz
stereo source, and actual CPU Whisper, CPU DDSP and CUDA native RVC in
one OBS filter chain (STT first). Each processed 3,600 ten-millisecond
packets, 1,728,000 audio frames, 1,800 DDSP hops, 70 RVC hops and two
recognized speech sequences, with no STT sample drops and finite nonzero
output. All 21,600 instrumented callback invocations across both runs
recorded zero intercepted allocations, frees, blocking locks or waits.

| Run | Effect | Wall p99 us | Wall max us | Thread CPU p99 us | Thread CPU max us |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | STT | 720.859 | 9261.277 | n/a | n/a |
| 1 | DDSP | 10.000 | 191.798 | n/a | n/a |
| 1 | RVC | 9.610 | 188.010 | n/a | n/a |
| 2 | STT | 159.079 | 4076.972 | 17.362 | 29.055 |
| 2 | DDSP | 14.910 | 6948.111 | 11.311 | 21.992 |
| 2 | RVC | 19.479 | 1075.587 | 11.561 | 24.696 |

Run 1 full OBS dispatch p99/max was 740.782/9279.134 us; run 2
380.234/6972.931 us. Zero of 7,200 dispatches exceeded the 10 ms
packet budget. In run 2, all 63 callbacks with wall duration >=100 us
spent more than half that duration off CPU (43 STT, 10 DDSP, 10 RVC).
This distinguishes elapsed time from executing CPU time but does not
identify the scheduler/kernel cause. System load was uncontrolled
(roughly 26 one-minute load average during the first test).
The CPU clock adds measurement overhead. These are short paced tests
in isolated libobs, not a deadline guarantee or an active-profile test.

Reproduce with tests/verify-rt-filter-chain.sh and the local assets
specified in its environment variables. It builds the shared-library
positive control and chain runner, then launches a bounded test.
No changes to production callbacks were needed in this pass.

## Fifth pass: YAMNet and live stem callback coverage

The scoped Linux/glibc interposer now wraps five production filter types.
The optional probe-enabled YAMNet and buffered-stem OBS fixtures reuse
their existing functional checks, and require observed callback invocations
and zero intercepted allocations, frees, blocking locks and waits.
The separate DSO positive control is also included in model-free Linux CI.

YAMNet used the documented SHA-256-matched MediaPipe Tasks 1.2.3 and
YAMNet float32 assets, with explicit CPU selection. Both 44.1 and 48 kHz
passed twice: 414 callback invocations per run, 521 model categories,
silence and sine classification, unchanged stereo audio, missing-model
fail-closed behavior, stale-score expiry and zero capture drops.
The callback count includes the intentionally invalid model instance.

Real buffered stem testing used a private CUDA HTDemucs sidecar (FP32,
zero shifts, 0.1 overlap, four-second segments), at 48 kHz. A preliminary
fixture made by repeating a 12-second music excerpt failed the preexisting
wet-output energy/stereo difference thresholds despite nonzero wet samples
and no intercepted RT violations. No acceptance thresholds were changed.
The earlier qualified 24-second fixture passed: 1,152,000 frames, initial
silence, distinct nonzero stereo vocal output, and all four stem choices
exposed. Actual eight-second worker windows completed in 1.802, 2.076
and 1.811 seconds. This does not establish sustained deadline guarantees.

| Path | Rate | Calls | Wall p99 us | CPU p99 us | CPU max us |
| --- | --- | ---: | ---: | ---: | ---: |
| YAMNet first | 48 kHz | 414 | 16.051 | 6.643 | 10.360 |
| YAMNet first | 44.1 kHz | 414 | 20.049 | 6.592 | 9.357 |
| YAMNet repeat | 48 kHz | 414 | 24.237 | 7.344 | 11.272 |
| YAMNet repeat | 44.1 kHz | 414 | 13.777 | 6.822 | 13.485 |
| Live stem preliminary fixture (functional fail) | 48 kHz | 2400 | 17.154 | 8.376 | 25.117 |
| Live stem qualified fixture | 48 kHz | 2400 | 15.431 | 8.486 | 20.458 |
| Live stem unavailable backend | 48 kHz | 1400 | 13.476 | 6.522 | 20.659 |

The missing-socket run also passed, producing silence across all 672,000
frames rather than misrepresenting dry audio as separated stems.
All 7,856 additional callback invocations recorded zero intercepted allocation, deallocation,
blocking lock or wait calls and no individual 10 ms callback overruns.
They were tested separately, not in the earlier three-filter chain.
Only test code changed; neither production filters nor active OBS
profile were modified. Reproduce using
`tests/verify-rt-extra-filters.sh [--with-stems]` and local assets
specified in the script. The stem worker uses a temporary private
socket and is stopped by the test runner.

## Sixth pass: optional Matchering and DeepFilter callback checks

The test-only interposer also supports Matchering and DeepFilter. Run
`SOUND_RT_PROBE=1 bash tests/verify-matchering-obs.sh` and
`SOUND_RT_PROBE=1 bash tests/verify-deepfilter-obs.sh /path/to/onnxruntime-src`.
The ordinary model-free test remains unchanged when the flag is absent.

| Filter | Calls | Wall p99 us | CPU p99 us | CPU max us |
| --- | ---: | ---: | ---: | ---: |
| Matchering | 203 | 132.502 | 24.205 | 27.762 |
| DeepFilter | 82 | 9.940 | 6.362 | 6.362 |

Both functional fixtures passed with zero intercepted allocations, frees,
blocking locks or waits. Matchering used a generated WAV reference and
200 additional wet packets. DeepFilter used synthetic ONNX gain graphs,
including model replacement and clearing, not a real noise suppression
model. All three Matchering wall-time spikes over 100 us spent most of
their duration off CPU. No production code changed.

## Repeatable tests

Model-free queue and DSP checks are included in the public CI verification
script. Additional local checks:

    bash tests/verify-matchering-obs.sh
    bash tests/verify-demucs-source-rt-obs.sh /path/to/onnxruntime-src
    bash tests/verify-demucs-filter-rt-obs.sh /path/to/build/_deps
    bash tests/verify-demucs-no-fake-stems.sh
    bash tests/verify-quarantine.sh
    bash tests/verify-deepfilter-obs.sh /path/to/onnxruntime-src

The DeepFilter integration script requires Python onnx to generate synthetic
models. It does not download models or install into the user's OBS profile.
Existing DDSP, RVC and STT model qualification instructions remain applicable.

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
- Actual DeepFilterNet model compatibility, noise suppression quality and
  sustained latency remain unqualified. Current transport recovery has been
  tested, but a real noise suppression model was not available locally.
- STT model qualification for other backends and the 44.1 kHz full pipeline
  was not repeated in this pass. Extend callback instrumentation to legacy
  opt-in filters and other production source paths, longer sustained
  measurements and additional allocator/lock APIs; further worker-side
  tensor allocation improvements remain candidates.
- Public hosted CI and other platforms have not been run for this checkpoint.
