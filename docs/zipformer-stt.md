# Zipformer CPU streaming and rolling captions

Inspired by [CaptionFlow](https://github.com/XWHQSJ/captionflow/tree/02d6961e363a1d70b795571decc631567c483667).
Sound's implementation uses its existing STT capture and publication path,
not a second filter. No CaptionFlow source was copied.

The optional backend accepts only the verified English 20M int8 transducer
from [Hugging Face](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17)
at revision d42f2d9f7ca24806fb667456a18a9f1b60f70d16. The four files total
43,649,301 bytes. See tests/fixtures/zipformer-en-20m-int8.json. File sizes
and SHA256 are checked before invoking the native recognizer to reduce the
risk of loading malformed models. This is a fixed model contract rather
than a general model catalog.

## Runtime
Sherpa-ONNX v1.12.40 CPU no-TTS Linux x64 shared library release:
[sherpa-onnx-v1.12.40-linux-x64-shared-no-tts-lib.tar.bz2](https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.12.40/sherpa-onnx-v1.12.40-linux-x64-shared-no-tts-lib.tar.bz2).
Archive SHA256:
`f5e1c393f5425e70fbcd66b2f8392b7e17f3e271bb7102cec29b7d5213a07afe`.
Matched header from the same source tag, SHA256
`437b1279047877167d8fadc74a60d47f3df514d703fdac1c1b6851da9bc2fdb4`.
The dependency version is ONNX Runtime 1.24.4. Sherpa's pinned dependency
archive from csukuangfj/onnxruntime-libs:
`onnxruntime-linux-x64-glibc2_17-Release-1.24.4.zip`, SHA256
`2cacf5084737b30f61a5c8a60efa0408b26f3e8ca6cc0dc312fe4d52d07724f9`.
Its original libonnxruntime.so matched the Sherpa archive byte for byte.
Keep the Sherpa LICENSE, ONNXRUNTIME-LICENSE, and
ONNXRUNTIME-THIRDPARTYNOTICES.TXT from that exact package.

The staging process uses patchelf to rename the ORT SONAME to
libsound-zipformer-onnxruntime.so, replace the Sherpa C library's matching
NEEDED entry, rename its SONAME to libsound-zipformer-sherpa.so and set
RUNPATH to $ORIGIN. Keep these two renamed binaries with the matched
c-api.h and notices in one directory. Generate SHA256SUMS covering the two
binaries and notices. This avoids a generic libonnxruntime filename
collision; coexistence with arbitrary loaded ONNX Runtime consumers
is not established by this alone.

Pass SOUND_ZIPFORMER_RUNTIME_DIR pointing to this directory to an explicitly
enabled STT build. Without it, there is no Zipformer registration in the
engine picker and no additional libcrypto dependency. OpenSSL verifies
weights at model load. The runtime is CPU-only. Model selection is a
directory picker, with one to eight CPU threads; the shared speech gate,
silence and maximum phrase settings still define finalization. No Python,
network inference or automatic model download is required.

## Rolling caption display
All STT engines support one to eight caption history lines (default one,
preserving prior behavior). The changing partial occupies the last line,
then the finalized result replaces it. These lines are for the selected
text source only; V1/V2 signals and the STT-to-TTS relay retain individual
recognition results. History clears on expiry, source/target lifecycle
and timestamp discontinuities. The history count applies on the next
published result, not via an immediate UI repaint. There is no transcript
file recorder or word-by-word styling in this change.

## Verification
The native probe loads the exact runtime and feeds a real 16 kHz JFK
fixture, checking changing partials and recognized words. In one local
warm-cache run with two CPU threads, context creation was 294 ms and
decoding the ~11-second recording took 158 ms with 18 distinct partials.
The model omitted/misrecognized the opening words; this is a lightweight
option, not a demonstrated accuracy improvement.

Real OBS tests (both 48 and 44.1 kHz) verify actual dry audio, expected
recognized words, caption routing, changed partials, rolling display across
two uninterrupted utterances, hot-update stability, silence, mute,
stopped-source flush, disable/missing-model recovery, and unchanged signal
semantics. See tests/verify-zipformer-obs.sh. Unified property layout and
configuration policy, model-free caption history, exact runtime packaging,
and zero-effect quarantine also passed. No arbitrary model loading,
hotwords, token-time signal, censorship, native endpointer, cross-backend
switching, simultaneous third-party ORT load or full expanded acceptance
is qualified by this checkpoint.

### Advanced decoding

Modified beam search, active paths (1–8, default 4) and blank
penalty (0–3, default 0) are available in the Zipformer advanced
property group. Greedy remains the default; beam paths apply only
to modified beam search. Changing these session options reloads
the recognizer. Actual OBS tests with beam 4 and blank penalty .1
passed both 44.1 and 48 kHz, including rolling partials and recovery.
Hotwords require the matching BPE vocabulary, absent from the
currently pinned package; no unsupported placeholder is shown.
