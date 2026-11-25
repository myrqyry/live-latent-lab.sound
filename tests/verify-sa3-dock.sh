#!/usr/bin/env bash
# Real Qt widgets with real libobs source and native model; no OBS profile edits.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_STAGE:?Set dock-enabled Sound staging directory}"
: "${SOUND_SA3_MODELS:?Set local verified model directory}"
work="$(mktemp -d /tmp/sound-sa3-dock-test.XXXXXX)"
trap 'rm -rf "$work"' EXIT
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -O2 -fPIC -pthread -I"$inc" -I"$root/src" \
    "$root/tests/verify-sa3-dock.cpp" $(pkg-config --cflags Qt6Widgets Qt6Test) \
    -std=c++17 $(pkg-config --libs Qt6Widgets Qt6Test libobs) -o "$work/test"
QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 40 "$work/test" \
    "$SOUND_SA3_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_SA3_STAGE/data" "$SOUND_SA3_MODELS" >"$work/test.log" 2>&1 || { cat "$work/test.log"; exit 1; }
grep -F 'SA3_DOCK_DRAFT_CONFLICT=1 SWITCH_BLOCKED=1' "$work/test.log"
grep -F 'SA3_DOCK discovered=1 selected=1 draft=1 saved=1 generated=1 live=1 isolated=1 removed=1 pass=1' "$work/test.log"
