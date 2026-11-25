#!/usr/bin/env bash
# Isolated real libobs acceptance, using an existing staged build and verified models.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_ZIPFORMER_STAGE:?Set Sound staged plugin directory}"
: "${SOUND_ZIPFORMER_MODELS:?Set the pinned English 20M int8 model directory}"
: "${SOUND_STT_FIXTURE:?Set the JFK speech WAV}"
work="$(mktemp -d /tmp/sound-zipformer-test.XXXXXX)"
trap 'rm -rf "$work"' EXIT
runtime="$SOUND_ZIPFORMER_STAGE/bin/64bit/stt-runtime/zipformer"
(cd "$runtime"; sha256sum -c SHA256SUMS)
module="$SOUND_ZIPFORMER_STAGE/bin/64bit/live-latent-lab-sound.so"
data="$SOUND_ZIPFORMER_STAGE/data"
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-stt-obs.cpp" \
 -Wl,--no-as-needed -lobs-frontend-api $(pkg-config --libs libobs) -Wl,--as-needed -o "$work/probe"
"${CXX:-c++}" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-stt-caption-history.cpp" -o "$work/history"
"$work/history"
for rate in 48000 44100; do
 ffmpeg -nostdin -v error -i "$SOUND_STT_FIXTURE" -ar "$rate" -ac 1 -f f32le "$work/$rate.raw"
 SOUND_TEST_ZIPFORMER=1 QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 80 "$work/probe" "$module" "$data" "$SOUND_ZIPFORMER_MODELS" "$work/$rate.raw" "$rate" > "$work/$rate.log" 2>&1 || { cat "$work/$rate.log"; exit 1; }
 grep -F 'SOUND_ZIPFORMER_PICKER=1' "$work/$rate.log"
 grep -F 'SOUND_ZIPFORMER_ROLLING=1' "$work/$rate.log"
 grep -F 'SOUND_STT_LIFECYCLE off=1 missing=1 recovered=1' "$work/$rate.log"
 grep -F "SOUND_STT_REAL rate=$rate " "$work/$rate.log" | grep -F 'pass=1'
done
