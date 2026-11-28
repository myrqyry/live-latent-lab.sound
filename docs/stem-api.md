# Sound stem interfaces v1

Compatibility baseline for the experimental Linux x86_64 `stem_live`
effect, originally baselined at 9c7499c with the optional overlap extension below. This describes the local Unix socket
and native OBS interfaces, not HTTP, a network API, or a promised stable
public release. See [expanded build and operation](expanded-build.md) and
[qualification evidence](../RELEASE_STATUS.md).

The file and resident inference backends use the same wire contract.
One request separates one stereo window into four stereo stems.
A producer filter can mix those stems or feed multiple ordinary OBS
sources. It does not send separate model requests for each subscriber.

## LLSTEM01 wire protocol

One request and one response per AF_UNIX SOCK_STREAM connection.
All header integers are unsigned 32-bit little-endian. The eight magic
bytes are ASCII, with no trailing NUL. Python format: `<8sIIIIIII`,
exactly 36 bytes (no padding). The current C++ PCM transport copies
native floats directly: little-endian IEEE-754 binary32 hosts are
required; big-endian hosts are not supported by this implementation.

| Byte offset | Field | Request | Successful response |
| --- | --- | --- | --- |
| 0 | magic (8 bytes) | LLSTEM01 | LLSTEM01 |
| 8 | version | 1 | 1 |
| 12 | rate | 44100 or 48000 Hz | Echo |
| 16 | frames | rate × 8 | Echo |
| 20 | channels | 2 | 2 |
| 24 | seq | Caller-supplied correlation ID | Echo |
| 28 | stems | 0 | 4 |
| 32 | status | 0 | 0 |

The current client increments seq per attempt and checks the echo.
It is a uint32 request identifier, not a timestamp, durable stream ID,
generation ID, or globally unique sequence. A server does not deduplicate
requests or cache outputs based on seq.

### PCM layout

The request payload immediately follows the header: contiguous planar
little-endian float32 samples, shape `[2, frames]`, left then right.
The response is shape `[4, 2, frames]` in C order; eight contiguous
planes: Drums L/R, Bass L/R, Other L/R, Vocals L/R. Within each plane
samples are chronological. No interleaving, length prefix, compression,
JSON, timestamps, or per-stem header. All audio samples must be finite.
The worker converts to the model's 44100 Hz geometry and back to the
requested rate. No promise of exact mix reconstruction or peak bounds
after resampling and summation.

| Rate | Frames | Request payload bytes | Success payload bytes |
| --- | ---: | ---: | ---: |
| 44100 | 352800 | 2822400 | 11289600 |
| 48000 | 384000 | 3072000 | 12288000 |

These counts exclude the 36-byte header. Offset of sample f in the
response payload is `4 * ((2 * stem + channel) * frames + f)` bytes.
Stem indexes: Drums=0, Bass=1, Other=2, Vocals=3; channel indexes L=0,
R=1. The wire header does not encode these names; the mapping is fixed
by the protocol version.

### Golden headers

The following exact hexadecimal byte strings are exercised by the
protocol regression tests. Example: 48000 Hz, 384000 frames, seq=45.

Request:
```text
4c4c5354454d3031 01000000 80bb0000 00dc0500 02000000 2d000000 00000000 00000000
```

Success response:
```text
4c4c5354454d3031 01000000 80bb0000 00dc0500 02000000 2d000000 04000000 00000000
```

Error after a valid request header:
```text
4c4c5354454d3031 01000000 80bb0000 00dc0500 02000000 2d000000 00000000 01000000
```

### Rejection, timing and local trust

- The server rejects wrong magic/version, unsupported rate, incorrect
  frame count, non-stereo shape, nonzero request stems/status,
  incomplete reads, nonfinite PCM, and invalid/incomplete separated
  results. On handled ValueError, EOFError, OSError or RuntimeError
  it attempts an error header (stems=0, status=1, no error payload).
  If header parsing failed, rate/frames/seq are zero. An incomplete
  connection, crash or failed write may produce no complete error
  header, including after a partial success response. Treat any
  incomplete or invalid transaction as failure.
- The client accepts only the exact success header, complete payload
  and finite samples in all eight planes, including unselected stems.
  Its worker uses an absolute 12-second transfer deadline with 20 ms
  polling and generation/shutdown checks. The server configures a
  30-second socket operation timeout, not a model execution deadline.
  Closing a connection does not interrupt an inference already running.
- The local endpoint defaults to `$XDG_RUNTIME_DIR/lll-sound-stems.sock`;
  the server also accepts an explicit --socket path. The server
  requires an existing immediate parent directory owned by the user,
  not a symlink and not group/world writable, refuses an existing
  endpoint, and sets the socket to 0600. The client checks path length,
  socket type using lstat, effective-user ownership and no group/other
  permission bits before connecting. No peer-credential handshake or
  cryptographic authentication; this is a trusted same-user interface,
  not a defense against hostile local processes or path replacement.
- The server is serial. It warms up before accepting connections; it
  does not guarantee concurrent inference, fairness, or deadlines for
  independent producer filters. It has no network listener.

Implementation: [Python sidecar](../tools/live_stem_sidecar.py) and
[C++ client](../src/framework/live-stem-client.hpp).

## Native OBS interfaces

Both registrations require the experimental `stem_live` build gate.

| Source ID | OBS type | Display name |
| --- | --- | --- |
| stem_live_buffered_filter | Audio filter | LLL.sound Buffered stem (experimental) |
| stem_live_output | Audio input | LLL.sound Stem Output |

The filter uses a fixed 12-second sample-count prefill (not a wall-clock
guarantee) with 8-second inference windows. Roughly four seconds remain
for the first window to be processed before playback. A missed playback
deadline invalidates the generation; late results never catch up by
playing stale audio. The 12-second socket transfer limit does not extend
that audio budget. Failures mute the active filter and child stems; the
external worker is user-owned and is not started or stopped by OBS.
Three automatic retry attempts use 250/500/1000 ms backoff; four
successful windows replenish the budget. Retry worker, a socket change,
disable/enable or detachment invalidate the prior generation. Disabling
or removing the filter lets OBS bypass it; original parent audio may
then be audible.

### Persisted OBS settings

| Filter key | Type | Default / meaning |
| --- | --- | --- |
| socket_path | string | Runtime directory default if available |
| smooth_windows | bool | false; optional 2 s cross-window overlap, 6 s hop |
| stem | int | 3 (Vocals); 0–3 in single-stem mode |
| mix_mode | int | 0 single, 1 instrumental, 2 custom, 3 separate outputs |
| drums_gain, bass_gain, other_gain, vocals_gain | double | 1.0; linear 0–2, quantized to 1/4096 |
| drums_mute, bass_mute, other_mute, vocals_mute | bool | false; custom mode |
| drums_solo, bass_solo, other_solo, vocals_solo | bool | false; custom mode |
| retry_worker | button | Explicitly resets retry budget; not a saved parameter |

Invalid stem indexes fall back to Vocals; unknown modes use single-stem
selection. Custom gain is clamped, nonfinite values resolve to zero.
Mute overrides solo. Mix changes apply at packet boundaries without
reloading the worker or prefill. Separate-output mode silences the
parent mix; children always receive raw separated stems, irrespective
of its custom gains. Use the children's own OBS mixer controls.

| Stem Output key | Type | Default / meaning |
| --- | --- | --- |
| filter_uuid | string | Empty/unbound; producer filter UUID (not parent source UUID) |
| stem | int | 3 (Vocals); 0–3 |

Nonempty binding IDs up to 80 bytes are accepted, but only a matching
producer UUID resolves. Direct audio-capable OBS inputs are eligible
producer parents; a Stem Output cannot itself be a fanout producer.
Do not create indirect audio feedback loops. A child can be created
before its parent attaches, and multiple children can select the same
stem. Rename-safe binding does not imply cross-collection UUID
stability. Child sources do not activate a missing/inactive parent.
No global OBS output slots are used.

### Read-only procedure signatures

Call on a live referenced **filter instance**, not on a Stem Output,
the parent input, a source-type ID, or a global broker. Use a control
or worker thread, not the audio callback. Both procedures are void
with no input parameters. Check proc_handler_call success and release
the caller-owned calldata. They do not consume audio or run inference.

```cpp
void sound_stem_live_v1(out string status, out int windows, out int automatic_retries, out int delay_ms)
void sound_stem_fanout_v1(out int packets, out int drops, out int subscribers)
```

| Procedure | Output | Type | Meaning |
| --- | --- | --- | --- |
| sound_stem_live_v1 | status | string | buffering, ready, error |
| sound_stem_live_v1 | windows | int | Cumulative successfully queued windows |
| sound_stem_live_v1 | automatic_retries | int | Cumulative automatic attempts initiated |
| sound_stem_live_v1 | delay_ms | int | 12000, configured sample-count delay |
| sound_stem_fanout_v1 | packets | int | Cumulative dispatch batches processed while allowed |
| sound_stem_fanout_v1 | drops | int | Cumulative failed dispatch reservations |
| sound_stem_fanout_v1 | subscribers | int | Latest dispatcher binding count when allowed, otherwise 0 |

Ready means a validated window was queued in the current generation,
not that prefill elapsed or that sound is audible. Error can be
transient during automatic recovery. A disabled/unattached filter is
not necessarily represented by a distinct status. The counters are
per-instance, not reset by mix changes, retries or socket reload.
Packets counts batches, not subscriber deliveries; it can include
silence and batches with no eligible recipient. Drops is not a count
of lost samples or missing recipient deliveries. Subscribers includes
bound children that may be disabled or not currently consuming audio.
Fanout fields are independently sampled atomics, not a transactional
snapshot or health certificate; a dispatcher failure may leave old
counter values. Use native OBS state and audio observation for actual
delivery. Integer values use OBS calldata integer accessors.

Minimal native reader (C++ fragment; caller holds a strong reference
to the filter until after the call):

```cpp
calldata_t data{};
if (proc_handler_call(obs_source_get_proc_handler(filter),
                      "sound_stem_fanout_v1", &data)) {
    auto packets = calldata_int(&data, "packets");
    auto drops = calldata_int(&data, "drops");
    auto subscribers = calldata_int(&data, "subscribers");
    // Copy/use diagnostic values here.
}
calldata_free(&data);
```

### Integration with the OBS ecosystem

Aftermarket already discovers non-filter audio sources by their OBS
UUID. Add Stem Output sources to the scene, then route/monitor them
as ordinary audio sources. HyperShaderFX discovers audio-capable
sources by UUID and captures their audio; choose the desired stem as
its audio source. No inference, custom IPC or C++ ABI is needed by
either consumer. These interfaces match by source inspection;
combined plugin runtime and real multitrack recording are not yet
qualified. The in-process transcript/classification/speech broker
is a separate interface: [Sound signal interop v1](sound-signal-interop-v1.md).

## Compatibility and verification

Preserve the v1 header, byte layout, stereo stem order, 8-second
request geometry, error semantics and existing procedure signatures.
The optional OBS-side cross-window blending keeps the v1 request
geometry at eight seconds and delay_ms at 12000. It sends a new request
every six seconds of capture, retaining two seconds of input context.
Only the first six seconds of each prediction are published; the final
two seconds are retained for complementary raised-cosine blending into
the next prediction. Both channels of every stem use the same weights
and timeline. The generation reset discards both input context and
output tail. Changing smooth_windows starts fresh prefill. The
windows counter counts successful requests, not seconds or played hops.
Any new request geometry or incompatible wire/OBS contract requires an
explicit new version and separate qualification; do not silently
reinterpret v1. For two-stem, six-stem and native GPU candidate
evidence, see [separation backend expansion](stem-model-families.md). Treat the existing 12-second delay and failure behavior
as the current playback contract, not a claim about the model's
minimum achievable latency.

Relevant tests: [Python protocol](../tests/test_live_stem_sidecar.py),
[C++ client](../tests/verify-stem-live-client.cpp),
[stereo recovery](../tests/verify-stem-recovery.cpp),
[shared mix](../tests/verify-stem-mix.cpp),
[dispatch](../tests/verify-stem-dispatch.cpp) and
[real OBS fanout](../tests/verify-stem-output-obs.cpp).
Run the isolated model-free suite with:

```sh
STEM_SPLITTER_PYTHON=/path/to/stem-splitter/.venv/bin/python \
  bash tests/verify-stem-live.sh
```

For actual model and OBS lifecycle testing, follow the fixture and
runtime requirements in [expanded-build.md](expanded-build.md).
Model-free tests do not establish inference performance or music
quality.
