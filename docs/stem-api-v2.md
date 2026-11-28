# Experimental named two-stem transport

LLSTEM02 profile 1 is an opt-in local worker contract used by the
experimental RoFormer OBS filter. LLSTEM01 remains unchanged.
Use a separate private AF_UNIX endpoint. No automatic protocol fallback.

Profile 1 is the pinned Kijai Mel-Band RoFormer FP16 weights, FP32 CUDA
execution through audio-separator 0.47.0. The full 2.8-second context
returns two stereo stems, Vocals (estimated) and Instrumental (residual).
No other profiles are currently implemented. Future profiles require
explicit geometry and output definitions; profile IDs are not arbitrary
model filenames or a general discovery protocol.

## Header and payload

One request and response per connection. 40-byte header, Python format
`<8s8I`, little-endian unsigned integers, followed by planar IEEE-754
float32 PCM. The C++ client assumes a little-endian host.

| Offset | Field | Request | Success response |
| --- | --- | --- | --- |
| 0 | magic | LLSTEM02 | LLSTEM02 |
| 8 | version | 2 | 2 |
| 12 | rate | 44100 or 48000 | echo |
| 16 | frames | rate * 28 / 10 | echo |
| 20 | channels | 2 | 2 |
| 24 | seq | caller correlation ID | echo |
| 28 | stems | 0 | 2 |
| 32 | status | 0 | 0 |
| 36 | profile | 1 | 1 |

Request planes: left, right. Response planes: vocals left/right,
instrumental left/right. At 44100 Hz, 123480 frames, request payload
987840 bytes, response 1975680 bytes. At 48000 Hz, 134400 frames,
request 1075200 bytes, response 2150400 bytes. All samples finite.
The model runs at 44100 Hz; 48000 Hz input/output is resampled and
residual instrumental is computed at the original request rate.

On handled errors, server attempts status=1, stems=0, no payload.
If the request header did not validate, rate/frames/seq are zero.
A failed or incomplete transaction must not publish any PCM.
C++ rejects mismatched version/profile/rate/geometry/sequence/stem
count/status and nonfinite response samples. Reuses v1 local socket
ownership and permission checks, cancellation and 12-second transfer
cap. A transfer cap is not an audio deadline. Server has a 30-second
socket-operation timeout, not a guaranteed inference timeout. Closing
the connection cannot interrupt inference already running.

## Experimental OBS playback

The `stem_live` effect gate now registers the separate producer filter
`stem_live_melband_filter` (**LLL.sound RoFormer stems (experimental)**)
and reuses the ordinary `stem_live_output` source. The existing Demucs
filter ID and default geometry are unchanged. Use the separate v2
socket (default `$XDG_RUNTIME_DIR/lll-sound-melband.sock`).

Profile 1 has immutable 2.8 s inference context, 700 ms output hop,
350 ms future lookahead and 140 ms complementary blend. History starts
zero-padded (1.75 s); the first request needs only 1.05 s of actual
capture, later requests each need .7 s of fresh input. The worker
publishes the central hop and retains the next .14 s of predictions
for the subsequent crossfade. Each prediction contains the full
context; the wire contract itself does not encode readout geometry.
All channels use the same cursor and weights. The two-stem ring stores
four actual planes; no imaginary drums/bass/other. The fixed playback
prefill is **2100 ms of captured samples** (not a hardware or complete
audio-device latency measurement). At steady state the model must keep
up with .7 s hops; faults invalidate the generation and mute.
Three automatic retries use 250/500/1000 ms backoff; 40 successful
responses replenish the budget. Retry worker and socket changes
start a new generation, discarding old history/predicted tails.
Model and GPU choice are explicit in the managed or advanced external
worker; no fallback. Automatic mode owns the private process. Cold
model loading precedes sample-count prefill; see [worker setup](stem-worker-setup.md).

The filter supports single stem, instrumental, custom mix
(level/mute/solo on vocals and instrumental) and separate outputs
(silent parent). Mixer changes do not rebuffer. Add ordinary Stem
Output sources bound by the same producer UUID and choose the two
supported stems; the parent selector disables incompatible choices.
Persisted output IDs: Vocals=3 (unchanged), Instrumental=4 (new).
The dispatcher rejects incompatible bindings, including an old
saved Drums/Bass/Other selection on this producer. It never
reinterprets a saved stem. Use native OBS routing on the two sources.
The parent must be active; disabling/removing the filter can expose
its original input, as with v1. No global output-slot ownership.

```cpp
void sound_stem_live_v2(out string status, out int windows, out int automatic_retries, out int delay_ms)
```

This read-only procedure on the new filter has the same counter
semantics as v1, but delay_ms=2100. The unchanged
`sound_stem_fanout_v1` procedure reports batches/drops/subscribers
on either filter. No global model discovery yet. See
[the readout experiment](stem-model-families.md) for the measured
context choice; quality against ground truth is still unqualified.

## Local use and qualification

Run using a dedicated Python environment containing audio-separator
0.47.0, compatible CUDA torch, numpy, scipy and safetensors. No model
downloads. The loader checks the pinned model hash and loads the
checkpoint strictly; no automatic CPU fallback.

```sh
python tools/live_stem_v2.py --model /path/to/MelBandRoformer_fp16.safetensors --socket /private/runtime/melband.sock
c++ -std=c++17 -O2 -pthread -Isrc tests/verify-stem-v2-client.cpp -o /tmp/stem-v2-client
/tmp/stem-v2-client /private/runtime/melband.sock
```

The C++ harness uses synthetic stereo tones, verifies nonzero outputs,
finite PCM and mixture reconstruction at both rates. It does not
establish separation quality or streaming latency. Real local RTX
2070 SUPER CUDA round trips passed, server processing .2994/.3002 s
at 44100/48000 respectively, including resampling where applicable.
The qualification reused an explicitly isolated existing safetensors
dependency, without installing packages. Existing v1 tests, v2
malformed input/reply tests and isolated OBS registry all passed.


For real OBS tests of a staged stem-enabled module:

```sh
SOUND_MELBAND_STAGE=/path/to/stage \
SOUND_MELBAND_MODEL=/path/to/MelBandRoformer_fp16.safetensors \
SOUND_MELBAND_FIXTURE=/path/to/24s-stereo-48k-f32.raw \
STEM_SPLITTER_PYTHON=/path/to/venv/bin/python SOUND_RT_PROBE=1 \
  bash tests/verify-stem-melband-obs.sh
```

The synthetic stereo fixture is interleaved little-endian float32.
The runner tests both project rates and socket reload recovery at
48 kHz. It uses real libobs source capture callbacks, not speaker
or recording latency. See RELEASE_STATUS.md for evidence and limits.

For automatic startup in OBS and optional diagnostics, see [stem worker setup](stem-worker-setup.md).
