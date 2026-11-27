#!/usr/bin/env bash
# Linux/glibc only. Run with an isolated, staged Sound build (never the active profile).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_RT_STAGE:?stage containing stt, ddsp and rvc}"
: "${SOUND_STT_MODEL:?local Whisper model}"
: "${SOUND_RT_SPEECH:?48 kHz mono float32 raw speech fixture containing the word country}"
: "${SOUND_DDSP_LIBRARY:?local LiteRT}"
: "${SOUND_DDSP_MODELS:?local qualified DDSP models}"
: "${SOUND_RVC_HELPER:?local qualified native RVC worker}"
: "${SOUND_RVC_LIBRARY:?local native RVC library}"
: "${SOUND_RVC_MODEL:?local RVC GGUF}"
work="$(mktemp -d /tmp/sound-rt-chain.XXXXXX)"
trap 'rm -rf "$work"' EXIT
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -rdynamic -I"$inc" \
    "$root/tests/verify-rt-filter-chain.cpp" $(pkg-config --libs libobs) -ldl -o "$work/chain"
"${CC:-cc}" -std=gnu11 -O2 -fno-builtin -fPIC -shared -pthread \
    "$root/tests/rt-probe-fixture.c" -o "$work/fixture.so"
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -rdynamic -I"$inc" \
    "$root/tests/verify-rt-probe.cpp" $(pkg-config --libs libobs) -ldl -o "$work/probe"
"$work/probe" "$work/fixture.so"
QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 115 "$work/chain" \
    "$SOUND_RT_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_RT_STAGE/data" \
    "$SOUND_STT_MODEL" "$SOUND_RT_SPEECH" "$SOUND_DDSP_LIBRARY" "$SOUND_DDSP_MODELS" \
    "$SOUND_RVC_HELPER" "$SOUND_RVC_LIBRARY" "$SOUND_RVC_MODEL"
