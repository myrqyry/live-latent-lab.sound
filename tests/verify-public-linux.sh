#!/usr/bin/env bash
# Qualify the actual public candidate module in isolated libobs.
set -euo pipefail
: "${SOUND_MEDIAPIPE_LIB:?Set path to a compatible MediaPipe 1.2.3 C runtime}"
: "${SOUND_YAMNET_MODEL:?Set path to a compatible YAMNet TFLite model}"
test -s "$SOUND_MEDIAPIPE_LIB"
test -s "$SOUND_YAMNET_MODEL"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cd "$root"
cmake --preset public-linux -B "$work/build"
cmake --build "$work/build" --parallel "${BUILD_JOBS:-4}"
cmake --install "$work/build" --prefix "$work/stage"
module="$work/stage/bin/64bit/live-latent-lab-sound.so"
inc="$(pkg-config --variable=includedir libobs)/obs"
read -ra libs <<< "$(pkg-config --libs libobs)"
cxx="${CXX:-c++}"
"$cxx" -std=gnu++17 -O2 -I"$inc" tests/verify-module-load.cpp -Wl,--no-as-needed -lobs-frontend-api "${libs[@]}" -Wl,--as-needed -o "$work/registry"
"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" tests/verify-yamnet-obs-audio.cpp "${libs[@]}" -o "$work/yamnet"
export QT_QPA_PLATFORM=offscreen
xvfb-run -a timeout 25 "$work/registry" "$module" "$work/stage/data" 'yamnet'
for rate in 48000 44100; do
    SOUND_YAMNET_TEST_RATE="$rate" xvfb-run -a timeout 35 "$work/yamnet" "$module" "$work/stage/data" "$SOUND_MEDIAPIPE_LIB" "$SOUND_YAMNET_MODEL"
done
echo SOUND_PUBLIC_PROFILE_PASS
