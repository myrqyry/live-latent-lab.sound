# Native Kokoro Q8 qualification

Measured on yuzu-mint, 2026-11-24, RTX 2070 SUPER (7,797 MiB
reported device memory). This is an alternative-backend qualification,
not an enabled Sound TTS backend.

## Pinned inputs

- CPU: https://github.com/0xShug0/audio.cpp commit
  `db1869d0d53c7839b79cf7c1a16d36cc20102307`.
  Build only `kokoro_tts`; no model downloads during inference.
- CUDA: upstream v0.9.6
  `audio-v0.9.6-bin-ubuntu-x64-cuda12.8-colab.tar.gz`, archive SHA256
  `0cd6629b4e328a21bcf4a2359fe7ed615e1dc562073d01c8723c45cb7b724ffe`.
  Server executable SHA256
  `03194177a8a7814add8ca011adb3cca94f8ed65644d046160780d8473aea3b05`.
  This release requires libnccl.so.2, which was supplied locally by the
  existing Chatterbox environment for this test only. CUDA graphs were
  disabled by the runtime on Turing.
- GGUF: https://huggingface.co/audio-cpp/audio.cpp-gguf/tree/047ec541cc3f7e50c583acd56e0177ae286c4078/Kokoro-82M-GGUF
  `kokoro-82m-q8_0.gguf`, 189550976 bytes, SHA256
  `9a4ca6176460cdc8c7845b804c0bbe85a4791ee3ffff568a3fdadbb708c63f47`.
  Upstream documents 54 voice packs; this test used two.
- Existing ONNX baseline: installed reader Kokoro FP16 model and voices,
  CPUExecutionProvider.

## Measured results

Same English sentence ("The sound of the rain on the window is a quiet
reminder to slow down."), af_heart, speed 1. Native CPU is a three-request
warm mean; CUDA and ONNX are three-request warm medians. Native CPU
timing is direct session; CUDA includes local HTTP; ONNX includes the
existing Engine.synthesize call. Versions and audio lengths differ:
this is practical latency, not controlled kernel or quality parity.

| Path | Warm ms | Output seconds | Peak memory observed |
| --- | ---: | ---: | --- |
| Native Q8 CPU, 8 threads | 1149.77 | 4.600 | 932176 KiB process RSS |
| Existing ONNX CPU | 1192.8 | 3.989 | 500752 KiB process RSS |
| Native Q8 CUDA | 227.9 | 4.575 | 922 MiB warm GPU allocation |

CUDA cold request was 963.7 ms, ONNX cold 2043.8 ms (including
model initialization, excluding interpreter/server startup).
Native CUDA speed 1.5 returned 3 seconds of audio; am_michael
returned 5.225 seconds with a different waveform. All produced
nonempty mono 24 kHz WAVs.

While the actual OBS live texture harness ran with a second source
generating 60 seconds of SA3 music at 32 steps, 12 Kokoro requests
completed with 246.4 ms median and 501.9 ms maximum. Live 44.1 kHz
processing produced 37 blocks, 486602 wet frames, 3346 ms startup
delay, zero dropped frames, zero failures, zero underruns. The SA3
competitor completed. This establishes bounded coexistence for the
tested workload, not arbitrary GPU saturation.

## Reproduce

Build CPU from a clean pinned checkout with dependencies provided
by upstream:
```sh
cmake -S . -B build-kokoro -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=kokoro_tts \
  -DENGINE_ENABLE_CUDA=OFF -DENGINE_ENABLE_VULKAN=OFF \
  -DENGINE_BUILD_WARMBENCH=ON
cmake --build build-kokoro --target kokoro_tts_warm_bench audiocpp_cli -j8
build-kokoro/bin/kokoro_tts_warm_bench --model "$MODEL" --backend cpu \
  --threads 8 --iterations 3 --warmup 1 \
  --text 'The sound of the rain on the window is a quiet reminder to slow down.' \
  --audio-out native-cpu.wav
```

For CUDA, verify the release digest and binary digest above before
extracting/running. Use upstream `examples/server/kokoro.json`, set
host 127.0.0.1, a free local port, model path, CUDA device 0, 8 threads,
ui=false, ui_management=false, max_loaded_models=1, and
idle_unload_ms=10000. Supply the matching CUDA/NCCL dependencies to
that process only. Start `audiocpp_server --config server.json --no-ui`.
POST to `/v1/audio/speech` with model=kokoro, voice=af_heart, the test
sentence as input, response_format=wav and speed=1. Repeat four times
and measure after the first request. Validate WAV format and duration.
Stop the isolated test server afterwards. Do not expose this
qualification server to other hosts.

For concurrent qualification, run the Sound
`tests/verify-same-live.cpp` probe against the qualified six-effect
stage with SAME_LIVE_44100=1 and SAME_LIVE_COMPETE=1 while sending
the speech requests. Use an isolated libobs/Xvfb environment.

## Decision

Retain the current CPU ONNX reader default: native CPU does not
establish a meaningful benefit for this machine and uses more host
memory in this test. Native CUDA is a compelling opt-in candidate.
Qualify a narrow native runtime built only with the needed models
(no unnecessary NCCL dependency), then integrate over a private
worker boundary using the existing reader contract. Preserve voice,
speed, phoneme, language, pause, retention and cancellation semantics
or expose unsupported options explicitly. No automatic silent
fallback to a different speech engine. Test actual OBS playback,
long requests, backend failure/recovery and subjective speech
quality before replacing the reader. No live backend was changed
during this qualification.

Local artifacts are under the external audio library
`Kokoro-82M-GGUF` and `audio.cpp-runtime/v0.9.6`; local logs and
audition WAVs in the Sound qualification cache. Model weights and
runtime binaries are not committed. The previously qualified live
SAME-S build at 37dbad4 was independently installed and passed
post-install registry and live-source tests.
