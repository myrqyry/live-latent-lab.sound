#!/usr/bin/env bash
# Isolated Matchering build and actual libobs audio filter path. No user install.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d /tmp/sound-matchering-XXXXXX)"
trap 'rm -rf "$work"' EXIT
cd "$root"
sdk_args=()
if [[ -n "${SOUND_OBS_SDK:-}" ]]; then
    sdk_args+=("-DOBS_HEADER_PATH=$SOUND_OBS_SDK/include/obs")
fi
cmake -S . -B "$work/build" -G Ninja "${sdk_args[@]}" \
    -DFETCH_OBS=OFF -DSOUND_EXPERIMENTAL=ON -DSOUND_EFFECTS=matchering \
    -DSOUND_TOOLS_MENU=OFF -DOBS_PLUGIN_DESTINATION=bin/64bit \
    -DOBS_DATA_DESTINATION=data -DCMAKE_BUILD_TYPE=Release
cmake --build "$work/build" --parallel 2
cmake --install "$work/build" --prefix "$work/stage"
python3 - "$work/reference.wav" <<'PY'
import math, struct, sys, wave
with wave.open(sys.argv[1], "wb") as f:
    f.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
    f.writeframes(b"".join(struct.pack("<hh", int(10000*math.sin(i*.1)),
                                      int(8000*math.cos(i*.07))) for i in range(44100)))
PY
inc="$(pkg-config --variable=includedir libobs)/obs"
probe_flags=()
if [[ "${SOUND_RT_PROBE:-0}" == 1 ]]; then
    probe_flags=(-DSOUND_RT_PROBE -rdynamic -ldl)
fi
"${CXX:-c++}" -std=gnu++17 -O2 -I"$inc" tests/verify-matchering-obs.cpp \
    -Wl,--no-as-needed -lobs-frontend-api $(pkg-config --libs libobs) \
    -Wl,--as-needed "${probe_flags[@]}" -o "$work/check"
QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 30 "$work/check" \
    "$work/stage/bin/64bit/live-latent-lab-sound.so" "$work/stage/data" "$work/reference.wav"
