#!/usr/bin/env bash
# RVC default-gate and missing-runtime safety only, no successful inference claim.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
temp="$(mktemp -d /tmp/lll-sound-rvc-safety-XXXXXX)"
trap 'rm -rf "$temp"' EXIT
cmake -S "$root" -B "$temp/build" -G Ninja -DFETCH_OBS=OFF \
    -DSOUND_EXPERIMENTAL=ON -DSOUND_EFFECTS=rvc \
    -DSOUND_TOOLS_MENU=OFF -DCMAKE_BUILD_TYPE=Release \
    -DOBS_PLUGIN_DESTINATION=bin/64bit -DOBS_DATA_DESTINATION=data
cmake --build "$temp/build" --parallel 4
cmake --install "$temp/build" --prefix "$temp/stage"
module="$temp/stage/bin/64bit/live-latent-lab-sound.so"
if ldd "$module" | grep -E 'not found|libonnxruntime|libaudiocpp|libcuda|libggml|libwhisper';then
    echo "RVC_SAFETY_FAILED: unexpected native dependency" >&2;exit 1
fi
cxx="${CXX:-c++}"
inc="$(pkg-config --variable=includedir libobs)/obs"
"$cxx" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-rvc-window.cpp" -o "$temp/window"
"$temp/window"
"$cxx" -std=gnu++17 -O2 -I"$inc" "$root/tests/verify-module-load.cpp" $(pkg-config --libs libobs) -o "$temp/registry"
QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 20 "$temp/registry" "$module" "$temp/stage/data" rvc
"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-rvc-native-obs.cpp" $(pkg-config --libs libobs) -o "$temp/audio"
python3 - "$temp/fixture.raw" <<'PYFIX'
import math,struct,sys
with open(sys.argv[1],'wb') as f:
    f.write(struct.pack('<48000f',*(.2*math.sin(2*math.pi*220*i/48000) for i in range(48000))))
PYFIX
SOUND_RVC_MISSING=1 QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 20 "$temp/audio" "$module" "$temp/stage/data" "$temp/missing" "$temp/missing" "$temp/missing" "$temp/fixture.raw"
echo "RVC_SAFETY_ONLY_PASS=1 (NO successful voice inference tested)"
