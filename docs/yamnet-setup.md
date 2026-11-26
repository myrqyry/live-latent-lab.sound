# Set up the YAMNet preview

The Linux baseline uses a separately acquired MediaPipe Tasks C runtime and
the official YAMNet float32 model. Start with CPU (recommended). The filter
classifies sound while preserving the source audio; it does not create a
caption overlay. No weights or MediaPipe runtime are shipped with Sound.

## Acquire and verify the local assets

Download the Linux x86_64 wheel from
[MediaPipe 1.2.3 on PyPI](https://pypi.org/project/mediapipe/1.2.3/):
`mediapipe-1.2.3-py3-none-manylinux_2_28_x86_64.whl`.
Keep its license/notices with the extracted runtime. The qualified wheel
is 43,342,798 bytes, SHA-256
`1fcda646cc7b96f73453acc416ce31d0fba287d0f1fc095865272a7ea6986497`.
Extract the single member `mediapipe/tasks/c/libmediapipe.so` using a ZIP
archive tool. No Python installation is needed for YAMNet.

Download the official [YAMNet float32 v1 TFLite model](https://storage.googleapis.com/mediapipe-models/audio_classifier/yamnet/float32/1/yamnet.tflite)
from Google. Keep the original model license/terms. Use a directory you own,
separate from the Sound plugin installation. Verify both files before use:

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| libmediapipe.so | 127908512 | `7ce8b23159d3fd4e8b727e35afbfb35f3d45d6bde1ec11cc01c316b3bf469f63` |
| yamnet.tflite | 4126810 | `4d8b4a53282dc83ef04e3e7dbc4fbc98082e34e44ed798e16c3a0cdd4c584faf` |

```sh
sha256sum /path/to/libmediapipe.so /path/to/yamnet.tflite
```

These hashes identify the exact local files tested on 2026-11-25.
Google's object metadata returned the same model byte count and MD5
as the tested local copy. A shorter cached file with the same filename
failed FlatBuffer verification; do not use incomplete downloads.

## Use in OBS

Build and stage the [baseline profile](../README.md#build-and-stage).
With OBS closed, copy only the staged Sound plugin files into its own
user plugin directory, normally
`~/.config/obs-studio/plugins/live-latent-lab-sound/` on native Linux.
Do not overwrite a working installation without a backup.
Restart OBS, add **LLL.sound Native YAMNet (experimental)** to an audio
source's filter list and specify the two files and CPU delegate.
If you change model/runtime/delegate, recreate the filter (the settings
are applied on creation). Missing or incompatible assets fail closed
while the original audio is preserved.

## Reproduce acceptance

```sh
SOUND_MEDIAPIPE_LIB=/path/to/libmediapipe.so \
SOUND_YAMNET_MODEL=/path/to/yamnet.tflite \
SOUND_YAMNET_DEVICE=cpu ./tests/verify-public-linux.sh
```

The isolated real-model runner checks registration, silence and sine-wave
classification, all 521 category signals, stale expiry, missing-model
fallback and stereo passthrough at both supported sample rates. It does
not modify OBS scenes or install the module. GPU/Auto delegation,
multi-filter load and sustained hardware-clock tests are separate.
