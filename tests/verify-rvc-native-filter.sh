#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_RVC_HEADER_ROOT:?Pinned audio.cpp include root}"
: "${SOUND_RVC_LIBRARY:?Native audio.cpp RVC C ABI library}"
: "${SOUND_RVC_MODEL:?Pinned RVC F16 GGUF}"
: "${SOUND_RVC_FIXTURE:?Synthetic or authorized speech mono 16k PCM16 WAV}"
: "${SOUND_RVC_STAGE:?Staged Sound module with RVC selected}"
work="$(mktemp -d /tmp/sound-rvc-native-verify.XXXXXX)"
trap 'rm -rf "$work"' EXIT
ulimit -c 0
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -O2 -I"$root/src" -I"$SOUND_RVC_HEADER_ROOT" "$root/tools/rvc-native-worker.cpp" -ldl -lcrypto -o "$work/worker"
"$cxx" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-rvc-window.cpp" -o "$work/window"
"$work/window"
timeout 100 python3 "$root/tests/verify-rvc-private.py" "$work/worker" "$SOUND_RVC_LIBRARY" "$SOUND_RVC_MODEL" "$SOUND_RVC_FIXTURE"
inc="$(pkg-config --variable=includedir libobs)/obs"
"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-rvc-native-obs.cpp" $(pkg-config --libs libobs) -o "$work/obs"
for rate in 48000 44100;do
 ffmpeg -nostdin -v error -i "$SOUND_RVC_FIXTURE" -ar "$rate" -ac 1 -f f32le "$work/$rate.raw"
 SOUND_RVC_RATE="$rate" QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 65 "$work/obs" "$SOUND_RVC_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_RVC_STAGE/data" "$work/worker" "$SOUND_RVC_LIBRARY" "$SOUND_RVC_MODEL" "$work/$rate.raw" >"$work/$rate.log" 2>&1 || { tail -45 "$work/$rate.log";exit 1; }
 grep -F 'pass=1' "$work/$rate.log"
done
echo SOUND_RVC_NATIVE_FILTER_PASS=1
