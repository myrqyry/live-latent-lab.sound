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
"$cxx" -std=c++17 -O2 -pthread -I"$root/src" "$root/tests/verify-rvc-recovery.cpp" -o "$work/recovery"
"$work/recovery"
timeout 100 python3 "$root/tests/verify-rvc-private.py" "$work/worker" "$SOUND_RVC_LIBRARY" "$SOUND_RVC_MODEL" "$SOUND_RVC_FIXTURE"
inc="$(pkg-config --variable=includedir libobs)/obs"
probe_flags=()
if [[ "${SOUND_RT_PROBE:-0}" == 1 ]]; then probe_flags=(-DSOUND_RT_PROBE -rdynamic -ldl);fi
"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-rvc-native-obs.cpp" $(pkg-config --libs libobs) "${probe_flags[@]}" -o "$work/obs"
for rate in 48000 44100;do
 ffmpeg -nostdin -v error -i "$SOUND_RVC_FIXTURE" -ar "$rate" -ac 1 -f f32le "$work/$rate.raw"
 SOUND_RVC_RATE="$rate" QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 65 "$work/obs" "$SOUND_RVC_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_RVC_STAGE/data" "$work/worker" "$SOUND_RVC_LIBRARY" "$SOUND_RVC_MODEL" "$work/$rate.raw" >"$work/$rate.log" 2>&1 || { tail -45 "$work/$rate.log";exit 1; }
 grep -F 'pass=1' "$work/$rate.log"
done
# Run serially: the helper has exclusive CUDA residency.
for mode in HOT_RELOAD CRASH_RECOVER IDLE_RECOVER; do
 env SOUND_RVC_RATE=48000 "SOUND_RVC_$mode"=1 QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 100 "$work/obs" "$SOUND_RVC_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_RVC_STAGE/data" "$work/worker" "$SOUND_RVC_LIBRARY" "$SOUND_RVC_MODEL" "$work/48000.raw" >"$work/$mode.log" 2>&1 || { tail -45 "$work/$mode.log";exit 1; }
 grep "RVC_$mode.*pass=1" "$work/$mode.log"
 grep '^RT_CALLBACK ' "$work/$mode.log" || true
done
SOUND_RVC_LEGACY=1 SOUND_RVC_MISSING=1 QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 25 "$work/obs" "$SOUND_RVC_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_RVC_STAGE/data" "$work/worker" "$SOUND_RVC_LIBRARY" "$SOUND_RVC_MODEL" "$work/48000.raw" >"$work/legacy.log" 2>&1 || { tail -45 "$work/legacy.log";exit 1; }
grep 'RVC_OBS.*status=error.*hops=0.*pass=1' "$work/legacy.log"
echo SOUND_RVC_NATIVE_FILTER_PASS=1
