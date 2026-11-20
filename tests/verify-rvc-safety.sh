#!/usr/bin/env bash
# RVC quarantine verification. These are NEGATIVE/safety tests only.
# No working RVC model set is present; never call this "voice conversion passed".
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
temp="$(mktemp -d /tmp/lll-sound-rvc-safety-XXXXXX)"
trap 'rm -rf "$temp"' EXIT

ort_args=()
ort_source="$root/build_x86_64/_deps/onnxruntime-src"
if [[ -r "$ort_source/include/onnxruntime_cxx_api.h" ]]; then
    ort_args+=("-DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME=$ort_source")
fi

cmake -S "$root" -B "$temp/build" -G Ninja -DFETCH_OBS=OFF \
    -DSOUND_EXPERIMENTAL=ON -DSOUND_EFFECTS=rvc \
    -DSOUND_TOOLS_MENU=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$temp/stage" \
    -DOBS_PLUGIN_DESTINATION=bin/64bit -DOBS_DATA_DESTINATION=data \
    "${ort_args[@]}"
cmake --build "$temp/build" --parallel 4
cmake --install "$temp/build" --prefix "$temp/stage" > "$temp/install.log"

module="$temp/stage/bin/64bit/live-latent-lab-sound.so"
test -r "$module"
if env -u LD_LIBRARY_PATH ldd "$module" | grep -E 'not found|libwhisper|libggml|libQt6'; then
    echo "RVC_SAFETY_FAILED: wrong or missing native dependency" >&2
    exit 1
fi

if [[ -r "$ort_source/include/onnxruntime_cxx_api.h" ]]; then
    ort="$ort_source"
else
    ort="$temp/build/_deps/onnxruntime-src"
fi

cxx="${CXX:-c++}"
inc="$(pkg-config --variable=includedir libobs)/obs"
libs="$(pkg-config --libs libobs)"
"$cxx" -std=c++17 -O2 -I"$root/src" -I"$ort/include" \
    "$root/tests/verify-rvc-contract.cpp" -L"$ort/lib" \
    -lonnxruntime -Wl,-rpath,"$ort/lib" -pthread -o "$temp/schema"
"$temp/schema" "$root/tests/fixtures/onnxruntime_non_rvc_sigmoid.onnx"

"$cxx" -std=gnu++17 -O2 -I"$inc" \
    "$root/tests/verify-module-load.cpp" \
    -Wl,--no-as-needed -lobs-frontend-api $libs -Wl,--as-needed \
    -o "$temp/registry"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 20 "$temp/registry" "$module" "$temp/stage/data" rvc

"$cxx" -std=gnu++17 -O2 -pthread -I"$inc" \
    "$root/tests/verify-rvc-dry-obs.cpp" $libs -o "$temp/audio"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 20 "$temp/audio" "$module" "$temp/stage/data"

echo "RVC_SAFETY_ONLY_PASS=1 (NO successful voice inference tested)"
