#!/usr/bin/env bash
# Reproduce the first deslop checkpoint with real libobs and ONNX Runtime.
# Disposable binaries and installation; never touches an OBS user/system install.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build_x86_64}"
temp="$(mktemp -d /tmp/lll-sound-verification-XXXXXX)"
trap 'rm -rf "$temp"' EXIT

ort="$build/_deps/onnxruntime-src"
if [[ ! -r "$ort/include/onnxruntime_cxx_api.h" ]]; then
  # A repair build may reuse a verified, previously downloaded dependency
  # through CMake's FETCHCONTENT_SOURCE_DIR_ONNXRUNTIME override.
  ort="$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_ONNXRUNTIME:PATH=//p' "$build/CMakeCache.txt" | head -n 1)"
fi
inc="$(pkg-config --variable=includedir libobs)/obs"
libs="$(pkg-config --libs libobs)"
cxx="${CXX:-c++}"
test -r "$ort/include/onnxruntime_cxx_api.h"
test -r "$ort/lib/libonnxruntime.so"
test -f "$build/CMakeCache.txt"
selected="$(sed -n 's/^SOUND_EFFECTS:STRING=//p' "$build/CMakeCache.txt" | head -n 1)"
if [[ -z "$selected" ]]; then
  echo "This is the experimental feature suite. For default zero-effect builds run ./tests/verify-quarantine.sh." >&2
  exit 2
fi

"$cxx" -std=c++17 -O2 -pthread -I"$root/src" \
    "$root/tests/verify-ring-buffer.cpp" -o "$temp/ring"
"$temp/ring"

"$cxx" -std=c++17 -O2 -pthread -I"$root/src" -I"$ort/include" \
    "$root/tests/verify-mic-engine.cpp" \
    -L"$ort/lib" -lonnxruntime -Wl,-rpath,"$ort/lib" -o "$temp/mic"
"$temp/mic"

"$cxx" -std=gnu++17 -O1 -pthread -I"$inc" -I"$root/src" -I"$ort/include" \
    "$root/tests/verify-audio-filters.cpp" \
    "$root/src/mic-filter.cpp" "$root/src/stts-filter.cpp" \
    "$root/src/deepfilter-filter.cpp" \
    -L"$ort/lib" -lonnxruntime $libs -Wl,-rpath,"$ort/lib" \
    -o "$temp/audio"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' timeout 20 "$temp/audio"

cmake --build "$build" --target live-latent-lab-sound --parallel 4
cmake --install "$build" --prefix "$temp/stage" >"$temp/install.log"
module="$temp/stage/bin/64bit/live-latent-lab-sound.so"
test -r "$module"
if env -u LD_LIBRARY_PATH ldd "$module" | grep -q 'not found'; then
    echo "FAIL: staging module has missing shared libraries" >&2
    exit 1
fi

"$cxx" -std=gnu++17 -O2 -I"$inc" \
    "$root/tests/verify-module-load.cpp" -Wl,--no-as-needed \
    -lobs-frontend-api $libs -Wl,--as-needed -o "$temp/loader"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
    timeout 20 "$temp/loader" "$module" "$temp/stage/data" "$selected"
echo "SOUND_FOUNDATIONS_PASS=1"
