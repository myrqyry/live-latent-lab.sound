# Dependency and distribution inventory

This is a source-preview inventory, not a legal clearance of all upstream assets.
The minimal Linux test archive contains only Sound's compiled YAMNet plugin,
locale/manifest, LICENSE and NOTICE. It does not contain a working classifier
runtime or model. The source tree contains one small negative ONNX test graph,
not production weights. Expanded installations remain local-only candidates.

| Component | Source/provisioning | Distribution decision |
| --- | --- | --- |
| OBS / libobs / frontend API | [OBS Studio](https://github.com/obsproject/obs-studio), CI pins 32.2.0 | Host SDK/system library, not bundled |
| YAMNet / MediaPipe 1.2.3 | [MediaPipe](https://github.com/google-ai-edge/mediapipe), [YAMNet](https://github.com/tensorflow/models/tree/master/research/audioset/yamnet); [verified setup](yamnet-setup.md), [contract](yamnet-async-obs.md) | User-supplied compatible runtime/model; exact artifact provenance/terms must be verified before bundling |
| Whisper / Parakeet / Nemotron / Zipformer | [Expanded build](expanded-build.md), [Zipformer pinned package](zipformer-stt.md) | External headers, runtimes and weights; retain upstream notices, no bundled distribution approval |
| Piper / Kokoro / Chatterbox / ONNX Runtime | [Expanded build](expanded-build.md), [native Kokoro](native-kokoro.md), [native Turbo](native-turbo.md) | Local worker environments, model/voice rights and dependencies separate |
| TikTok / Edge | Ottsy third-party HTTPS and Edge online service; [expanded build](expanded-build.md) | Cloud service availability and terms separate; speech relay requires consent |
| HTDemucs / Torch | [Demucs](https://github.com/facebookresearch/demucs), [expanded build](expanded-build.md) | Separate worker environment and weights; no CUDA/Torch redistribution |
| Stable Audio 3 / SAME-S | [Audio Diffusion](audio-diffusion.md), [SA3 pinned qualification](sa3-native-qualification.md) | External runtime and model rights; SA3 runtime MIT notice is not a model license |
| DDSP / LiteRT | [DDSP pinned model/runtime provenance](ddsp.md) | External model pack and LiteRT headers/runtime |
| RVC / audio.cpp / FAISS | [RVC pinned qualification](native-rvc-qualification.md), [local checkpoints](native-rvc.md) | Separate worker/runtime; individual voice/checkpoint rights unresolved; trusted files only |
| Qt, FFmpeg, libcurl, OpenSSL | System development/runtime dependencies for selected expanded effects | No bundling in minimal package; exact linked-library terms and notices before expanded redistribution |
| Historical quarantined adapters | CMake fetch declarations for ONNX Runtime, Whisper, Eigen, demucs.onnx, pocketfft and JSON | Excluded from qualified preview profile; not validated release dependencies |

Before redistributing an expanded binary, inventory the exact versions, source
revisions, hashes, licenses, notices, corresponding source obligations, model
terms and provenance of every packaged file. A build that passes inference
tests does not establish redistribution rights. Never package the user's
local voice library or credentials. Do not expose untrusted worker executable
or model paths to remote callers.

The shared CI job exercises the baseline registration, quarantine and
model-free regressions; its short-lived test artifacts are not releases and
do not qualify expanded runtime installation. No CI job downloads model weights
or calls speech cloud services. Full local provisioning and the combined
acceptance run remain outstanding.
