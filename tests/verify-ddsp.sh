#!/usr/bin/env bash
# Real native CPU models + libobs audio. No downloads, installation or user profile changes.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_DDSP_INCLUDE:?Pinned LiteRT 2.4.0 C headers}"
: "${SOUND_DDSP_LIBRARY:?Qualified libLiteRt.so}"
: "${SOUND_DDSP_MODELS:?Qualified DDSP model directory}"
: "${SOUND_DDSP_STAGE:?Stage with ddsp enabled}"
work="$(mktemp -d /tmp/sound-ddsp-verify.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cxx="${CXX:-c++}"
for test in synthesis pipeline native;do
 "$cxx" -std=c++17 -O2 -I"$root/src" -I"$SOUND_DDSP_INCLUDE" "$root/tests/verify-ddsp-$test.cpp" -ldl -o "$work/$test"
 "$work/$test" "$SOUND_DDSP_LIBRARY" "$SOUND_DDSP_MODELS"
done
inc="$(pkg-config --variable=includedir libobs)/obs"
"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-ddsp-obs.cpp" $(pkg-config --libs libobs) -o "$work/obs"
module="$SOUND_DDSP_STAGE/bin/64bit/live-latent-lab-sound.so"
data="$SOUND_DDSP_STAGE/data"
for rate in 44100 48000;do
 SOUND_DDSP_RATE="$rate" QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 35 "$work/obs" "$module" "$data" "$SOUND_DDSP_LIBRARY" "$SOUND_DDSP_MODELS" >"$work/$rate.log" 2>&1 || { cat "$work/$rate.log";exit 1; }
 grep 'DDSP_OBS.*pass=1' "$work/$rate.log"
done
SOUND_DDSP_MISSING=1 SOUND_DDSP_RECOVER_MODELS="$SOUND_DDSP_MODELS" QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 20 "$work/obs" "$module" "$data" "$SOUND_DDSP_LIBRARY" "$work/nonexistent" >"$work/recovery.log" 2>&1 || { cat "$work/recovery.log";exit 1; }
grep 'DDSP_RECOVER_MODELS=1' "$work/recovery.log"
grep 'DDSP_MISSING.*pass=1' "$work/recovery.log"
echo SOUND_DDSP_VERIFIED=1
