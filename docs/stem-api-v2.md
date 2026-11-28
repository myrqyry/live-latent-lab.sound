# Experimental named two-stem transport

LLSTEM02 profile 1 is an opt-in local worker contract, not yet an OBS
source/filter or a live playback protocol. LLSTEM01 remains unchanged.
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

No global model discovery, independent stem mixing, cross-window
readout, or latency scheduling has been added to OBS for this profile.
The balanced candidate (2.8 s context, .7 s hop, .35 s future,
.14 s blend) is an experiment documented in stem-model-families.md,
not the wire hop or advertised end-to-end latency.

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
