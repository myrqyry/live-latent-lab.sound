#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_STAGE:?Set staged Sound plugin directory}"
: "${SOUND_SA3_MODELS:?Set verified local SA3 models directory}"
work="$(mktemp -d /tmp/sound-sa3-buffer-test.XXXXXX)"
trap 'rm -rf "$work"' EXIT
module="$SOUND_SA3_STAGE/bin/64bit/live-latent-lab-sound.so"
data="$SOUND_SA3_STAGE/data"
"${CXX:-c++}" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-sa3-buffer-policy.cpp" -o "$work/policy"
"$work/policy"
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-sa3-continuous.cpp" $(pkg-config --libs libobs) -o "$work/continuous"
export QT_QPA_PLATFORM=offscreen SOUND_SA3_ADAPTIVE=1
for rate in 48000 44100; do
    args=()
    if [[ "$rate" == 44100 ]]; then args=(SOUND_DIFFUSION_TEST_44100=1 SOUND_SA3_COMPETE=1); fi
    env -u SOUND_DIFFUSION_TEST_44100 -u SOUND_SA3_COMPETE -u SOUND_SA3_SOAK -u SOUND_SA3_PREFILL_CANCEL "${args[@]}" xvfb-run -a timeout 75 "$work/continuous" "$module" "$data" "$SOUND_SA3_MODELS" >"$work/$rate.log" 2>&1 || { cat "$work/$rate.log";exit 1; }
    grep -F '[sa3] backend: CUDA0' "$work/$rate.log" | head -1
    grep -F 'SA3_ADAPTIVE_BUFFER prefilled=1' "$work/$rate.log" | grep -F 'pass=1'
    grep -F "SA3_CONTINUOUS rate=$rate " "$work/$rate.log" | grep -F 'pass=1'
    if [[ "$rate" == 44100 ]]; then grep -F 'SA3_BUFFER_COMPETITOR started=1 completed=1' "$work/$rate.log";fi
done
env -u SOUND_DIFFUSION_TEST_44100 -u SOUND_SA3_COMPETE SOUND_SA3_PREFILL_CANCEL=1 xvfb-run -a timeout 40 "$work/continuous" "$module" "$data" "$SOUND_SA3_MODELS" >"$work/cancel.log" 2>&1 || { cat "$work/cancel.log";exit 1; }
grep -F 'SA3_PREFILL_CANCEL in_prefill=1 clean=1 recovered=1' "$work/cancel.log" | grep -F 'pass=1'
