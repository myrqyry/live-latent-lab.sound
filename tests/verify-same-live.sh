#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_STAGE:?Set live SAME-S enabled stage}"
: "${SOUND_SA3_MODELS:?Set qualified local SA3 model directory}"
work="$(mktemp -d /tmp/sound-same-live-test.XXXXXX)"
trap 'rm -rf "$work"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-same-overlap.cpp" -o "$work/overlap"
"$work/overlap"
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-same-live.cpp" $(pkg-config --libs libobs) -o "$work/live"
module="$SOUND_SA3_STAGE/bin/64bit/live-latent-lab-sound.so"
data="$SOUND_SA3_STAGE/data"
for test in default compete soak stall; do
    args=()
    case "$test" in
        compete) args=(SAME_LIVE_44100=1 SAME_LIVE_COMPETE=1) ;;
        soak) args=(SAME_LIVE_SOAK=1) ;;
        stall) args=(SAME_LIVE_STALL=1) ;;
    esac
    env -u SAME_LIVE_44100 -u SAME_LIVE_COMPETE -u SAME_LIVE_SOAK -u SAME_LIVE_STALL -u SAME_LIVE_MUSIC_RAW -u SAME_LIVE_RECORD_PREFIX QT_QPA_PLATFORM=offscreen "${args[@]}" xvfb-run -a timeout 60 "$work/live" "$module" "$data" "$SOUND_SA3_MODELS" >"$work/$test.log" 2>&1 || { cat "$work/$test.log";exit 1; }
    grep -F 'SAME_LIVE rate=' "$work/$test.log" | grep -F 'pass=1'
    if [[ "$test" == compete ]];then grep -F 'SAME_LIVE_COMPETITOR started=1 completed=1 failed=0' "$work/$test.log";fi
    if [[ "$test" == stall ]];then grep -F 'SAME_LIVE_STALL failed_closed=1 recovered=1' "$work/$test.log";fi
done
