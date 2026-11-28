#!/usr/bin/env bash
# Opt-in actual CUDA tests. No pre-launched worker or changes to user OBS.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_STEM_STAGE:?Set staged stem-enabled module}"
: "${SOUND_STEM_PROFILE:?Set existing mode-0600 stems.json}"
: "${SOUND_STEM_FIXTURE:?Set 24 s interleaved stereo 48 kHz f32 raw fixture}"
work="$(mktemp -d /tmp/sound-managed-obs-XXXXXX)"
mkdir -p "$work/config/live-latent-lab-sound"
install -m600 "$SOUND_STEM_PROFILE" "$work/config/live-latent-lab-sound/stems.json"
export XDG_CONFIG_HOME="$work/config" QT_QPA_PLATFORM=offscreen
inc="$(pkg-config --variable=includedir libobs)/obs"
read -r -a libs <<< "$(pkg-config --libs libobs)"
probe=()
if [[ "${SOUND_RT_PROBE:-0}" == 1 ]];then probe=(-rdynamic -DSOUND_RT_PROBE -ldl);fi
for type in output melband;do
    "${CXX:-c++}" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-stem-$type-obs.cpp" "${libs[@]}" "${probe[@]}" -o "$work/$type"
done
for type in output melband;do
    extra=()
    if [[ "$type" == output ]];then extra=(SOUND_STEM_SMOOTH=1);fi
    env "${extra[@]}" xvfb-run -a timeout 60 "$work/$type" \
        "$SOUND_STEM_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_STEM_STAGE/data" \
        "$SOUND_STEM_FIXTURE" --managed >"$work/$type.log" 2>&1 || { tail -50 "$work/$type.log";exit 1; }
    grep -E '^STEM_OUTPUT |^DEMUCS_MANAGED|^STEM_FANOUT_OBS|^MELBAND_|^RT_CALLBACK' "$work/$type.log"
done
echo "MANAGED_STEM_OBS_PASS=1 evidence=$work"
