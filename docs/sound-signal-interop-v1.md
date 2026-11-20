# Sound signal interop v1

The Sound family shares versioned OBS procedures, following Sight's
inventory/snapshot approach. No extra model instance, shared C++ ABI,
network port, caption-text intermediary, or standalone receiver source
is needed. The current contract is in-process, typed, pull-based fanout;
it is not a generic network pub/sub bus or durable event log.

## User-facing routing

On an ordinary input, attach **Live Latent: STT** and choose a model.
In **Live Latent: TTS**, Speech input -> **STT filter (speech-to-speech)**
lists compatible providers discovered from the same shared inventory
used by external plugins. Parent/filter UUIDs are stable channel IDs;
the visible label is the input/filter's display name. Renaming does
not break a connection. Different instances cannot collide even with
identical display labels. No arbitrary user-entered channel aliases
in this first version.

**Other plugin (text signal)** makes the TTS source an explicit writable
destination. It does not generate audio until a caller submits text.
Select any backend and voice. Local synthesis is the default allowed
path. Edge/TikTok require the separate cloud permission checkbox.
Neither selecting a destination nor reading a snapshot grants permission
to send microphone-derived text to a remote service.

The direct STT link speaks only new finals in sequence (8 queued max).
The external mode accepts explicit calls, with caller-owned event
deduplication. Both modes are stopped by OBS disable/deactivate or
Stop speech; backend/voice/input changes clear queued work. Direct
TTS-as-STT input is rejected; acoustic or virtual loopback is not detected.

## Inventory and read boundary

C++17 clients may include `src/interop/sound_signal_client.hpp`.
Only libobs is needed, no linkage to Sound itself. A client can enumerate
with `sound_signals::discover()`, then read and enqueue by stable
channel. The same common code drives the built-in STT picker, rather
than reimplementing discovery in every consumer.

C or C++ clients may call these OBS procedures on a referenced existing
Sound STT, TTS or native YAMNet endpoint. For inventory, pass
caller-owned `obs_data_t*` in the calldata `inventory` field:
`sound_signal_list_v1`. Result contains integer version=1 and
`channels` array, with channel, label, kind, enabled, relay_safe,
read_proc and write_proc. Kinds: transcript, classification, speech.
Discoverable does not imply ready, writable, or active. A disabled
entry remains discoverable but reads unavailable. No file transfer,
model load, or scene changes in discovery.

`sound_signal_read_v1` inputs:
- channel: UUID for TTS; parent UUID/filter UUID for STT and YAMNet.
- stream: final or latest (transcript), latest or category (classification),
  latest (speech status).
- category: category name when reading a classification category score;
  empty/omitted otherwise.

Outputs: kind, connected, available, status, text, final, sequence,
revision, label, score, completed, failed. Check kind and available
before using data. Transcript final snapshots use the existing v1
complete utterance contract; latest uses v2 live revisions. Revision
advances for changed partials and finals, sequence only for complete
phrases. Classification latest sequence is the number of inferred
windows; per-category score uses the existing category lookup.
Speech completed and failed counters report actual synthesis outcomes,
not delivery acceptance. Existing individual producer procedures
remain supported, including YAMNet's category inventory.

Reads copy snapshots, do not consume them or invoke inference.
Multiple independent clients can read the same channel. Unknown IDs,
disabled/detached filters, missing procedures, mismatched stream types
and expired results fail closed. Retain saved IDs rather than silently
picking a replacement. Discovery is for properties/control threads;
call snapshot reads on control/worker paths, never on the realtime audio
callback. The existing TTS polling path checks every 100 ms and
revalidates the owning input/filter. It remains latest-value delivery,
not lossless phrase history.

## Text reception

`sound_tts_enqueue_v1(in string text, out bool accepted)` called on
the selected TTS source, or `sound_signals::enqueue(channel,text)`.
Only 1–16384 bytes of nonempty valid UTF-8 accepted. Requires active,
enabled destination set to Other plugin mode, valid backend, available
queue slot, and cloud consent if applicable. The returned bool means
queued, not audio delivered. Callers must not blindly retry accepted
messages. Queue is bounded FIFO, caller owns external event IDs.
No arbitrary external network access. Intended for trusted in-process
plugins, not isolation against hostile native code.


This API is available in the opt-in public-linux-expanded profile. The independent native OBS client test passed; see RELEASE_STATUS.md for the combined qualification boundary.
