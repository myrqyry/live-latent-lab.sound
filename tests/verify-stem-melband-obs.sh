#!/usr/bin/env bash
# Real opt-in RoFormer acceptance against an already staged module.
# No downloads, system/profile modifications or user process interruption.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_MELBAND_STAGE:?Set candidate module stage}"
: "${SOUND_MELBAND_MODEL:?Set pinned local Mel-Band model}"
: "${SOUND_MELBAND_FIXTURE:?Set 24 s 48 kHz stereo interleaved f32 fixture}"
: "${STEM_SPLITTER_PYTHON:?Set local model Python interpreter}"
test -s "$SOUND_MELBAND_STAGE/bin/64bit/live-latent-lab-sound.so"
test -s "$SOUND_MELBAND_MODEL" && test -s "$SOUND_MELBAND_FIXTURE" && test -x "$STEM_SPLITTER_PYTHON"
work="$(mktemp -d /tmp/lll-melband-verify-XXXXXX)"
pid=""
cleanup(){
    if [[ -n "$pid" ]];then kill "$pid" 2>/dev/null || true;wait "$pid" 2>/dev/null || true;fi
    printf 'Evidence directory: %s\n' "$work"
}
trap cleanup EXIT
inc="$(pkg-config --variable=includedir libobs)/obs"
read -r -a libs <<< "$(pkg-config --libs libobs)"
probe=()
if [[ "${SOUND_RT_PROBE:-0}" == 1 ]];then probe=(-rdynamic -DSOUND_RT_PROBE -ldl);fi
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-stem-melband-obs.cpp" "${libs[@]}" "${probe[@]}" -o "$work/obs"
HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 "$STEM_SPLITTER_PYTHON" \
    "$root/tools/live_stem_v2.py" --model "$SOUND_MELBAND_MODEL" --socket "$work/model.sock" >"$work/model.log" 2>&1 &
pid=$!
for ((i=0;i<200;i++));do
    [[ -S "$work/model.sock" ]] && break
    kill -0 "$pid" || { tail -35 "$work/model.log";exit 1; }
    sleep .2
done
test -S "$work/model.sock"
for mode in 48000 44100 recovery;do
    extra=()
    if [[ "$mode" == recovery ]];then extra=(SOUND_MELBAND_RECOVER=1);elif [[ "$mode" == 44100 ]];then extra=(SOUND_MELBAND_44100=1);fi
    env "${extra[@]}" QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 40 "$work/obs" \
        "$SOUND_MELBAND_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_MELBAND_STAGE/data" \
        "$SOUND_MELBAND_FIXTURE" "$work/model.sock" >"$work/$mode.log" 2>&1 || { tail -50 "$work/$mode.log";exit 1; }
    grep -E '^MELBAND_OBS|^RT_CALLBACK' "$work/$mode.log"
done
echo SOUND_MELBAND_REAL_OBS_PASS=1
