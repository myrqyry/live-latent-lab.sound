#!/usr/bin/env bash
# Verify the quarantined microphone DSP independently of every neural effect.
# No ONNX/Whisper downloads, no OBS user installation, no audio-device recording.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d /tmp/lll-sound-mic-dsp-XXXXXX)"
trap 'rm -rf "$work"' EXIT

cmake -S "$root" -B "$work/build" -G Ninja \
    -DFETCH_OBS=OFF -DSOUND_EXPERIMENTAL=ON -DSOUND_EFFECTS=mic \
    -DSOUND_TOOLS_MENU=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DOBS_PLUGIN_DESTINATION=bin/64bit -DOBS_DATA_DESTINATION=data \
    -DCMAKE_INSTALL_PREFIX="$work/stage"
cmake --build "$work/build" --parallel 4
cmake --install "$work/build" --prefix "$work/stage" > "$work/install.log"

module="$work/stage/bin/64bit/live-latent-lab-sound.so"
test -f "$module"
if env -u LD_LIBRARY_PATH ldd "$module" | grep -E 'not found|libonnxruntime|libwhisper|libggml|libQt6'; then
    echo "MIC_DSP_FAIL: unexpected shared-library dependency" >&2
    exit 1
fi

cxx="${CXX:-c++}"
inc="$(pkg-config --variable=includedir libobs)/obs"
libs="$(pkg-config --libs libobs)"

"$cxx" -std=c++17 -O2 -I"$root/src" \
    "$root/tests/verify-mic-dsp-engine.cpp" -o "$work/core"
"$work/core"

"$cxx" -std=gnu++17 -O2 -I"$inc" \
    "$root/tests/verify-module-load.cpp" \
    -Wl,--no-as-needed -lobs-frontend-api $libs -Wl,--as-needed \
    -o "$work/registry"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 20 "$work/registry" "$module" "$work/stage/data" mic

"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" \
    "$root/tests/verify-mic-obs-audio.cpp" $libs -o "$work/audio"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 20 "$work/audio" "$module" "$work/stage/data"

echo "MIC_DSP_ISOLATED_PASS=1"
