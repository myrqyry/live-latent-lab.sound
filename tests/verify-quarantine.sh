#!/usr/bin/env bash
# Reproducible zero-effect OBS module regression. No model downloads, AI
# backends, OBS profile edits, or user/system plugin installation.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d /tmp/sound-effect-quarantine-XXXXXX)"
trap 'rm -rf "$work"' EXIT

cmake -S "$root" -B "$work/build" -G Ninja \
    -DFETCH_OBS=OFF \
    -DSOUND_EFFECTS= \
    -DSOUND_EXPERIMENTAL=OFF \
    -DSOUND_TOOLS_MENU=OFF \
    -DOBS_PLUGIN_DESTINATION=bin/64bit \
    -DOBS_DATA_DESTINATION=data \
    -DCMAKE_INSTALL_PREFIX="$work/stage"
cmake --build "$work/build" --parallel 4
cmake --install "$work/build" --prefix "$work/stage"

module="$work/stage/bin/64bit/live-latent-lab-sound.so"
if ! test -r "$module"; then
    echo "QUARANTINE_FAIL: missing staged module" >&2
    exit 1
fi
if env -u LD_LIBRARY_PATH ldd "$module" | grep -E 'not found|libonnxruntime|libwhisper|libggml|libQt6'; then
    echo "QUARANTINE_FAIL: default module has missing/unexpected AI/UI runtime dependencies" >&2
    exit 1
fi

inc="$(pkg-config --variable=includedir libobs)/obs"
cxx="${CXX:-c++}"
"$cxx" -std=gnu++17 -O2 -I"$inc" \
    "$root/tests/verify-module-load.cpp" \
    -Wl,--no-as-needed -lobs-frontend-api $(pkg-config --libs libobs) \
    -Wl,--as-needed -o "$work/check"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 30 "$work/check" "$module" "$work/stage/data" ""

# The gating contract must actively reject accidental effect enablement.
if cmake -S "$root" -B "$work/rejected" -G Ninja \
    -DFETCH_OBS=OFF -DSOUND_EFFECTS=mic -DSOUND_EXPERIMENTAL=OFF \
    >"$work/unsafe-config.log" 2>&1; then
    echo "QUARANTINE_FAIL: unsafe mic build was accepted" >&2
    exit 1
fi
if ! grep -q "Quarantine:" "$work/unsafe-config.log"; then
    echo "QUARANTINE_FAIL: rejected mic build for an unexpected reason" >&2
    tail -35 "$work/unsafe-config.log" >&2
    exit 1
fi
echo "SOUND_QUARANTINE_PASS=1"
