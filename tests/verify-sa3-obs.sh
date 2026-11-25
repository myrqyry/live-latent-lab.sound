#!/usr/bin/env bash
# Run against an explicitly staged opt-in diffusion build, not the active OBS profile.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_STAGE:?Set staged Sound plugin root}"
: "${SOUND_SA3_MODELS:?Set verified small-music model directory}"
work="$(mktemp -d /tmp/sound-sa3-obs.XXXXXX)"
trap 'rm -rf "$work"' EXIT
module="$SOUND_SA3_STAGE/bin/64bit/live-latent-lab-sound.so"
data="$SOUND_SA3_STAGE/data"
(cd "$SOUND_SA3_STAGE/bin/64bit/sa3-runtime"; sha256sum -c SHA256SUMS)
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-sa3-obs.cpp" $(pkg-config --libs libobs) -o "$work/probe"
export QT_QPA_PLATFORM=offscreen
for rate in 48000 44100; do
    args=()
    if [[ "$rate" == 44100 ]]; then args=(SOUND_DIFFUSION_TEST_44100=1); fi
    env -u SOUND_DIFFUSION_TEST_44100 "${args[@]}" xvfb-run -a timeout 70 "$work/probe" "$module" "$data" "$SOUND_SA3_MODELS" > "$work/$rate.log" 2>&1 || { cat "$work/$rate.log"; exit 1; }
    grep -F '[sa3] backend: CUDA0' "$work/$rate.log"
    grep -F 'DIFFUSION_DISABLED=1' "$work/$rate.log"
    grep -F 'DIFFUSION_OBS generated=1 audio=1' "$work/$rate.log"
done
