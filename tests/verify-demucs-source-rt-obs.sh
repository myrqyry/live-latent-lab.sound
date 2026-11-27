#!/usr/bin/env bash
# Compile real source callbacks against local libobs/ORT; no models or OBS installation.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ort="${1:?usage: verify-demucs-source-rt-obs.sh /path/to/onnxruntime}"
ort="$(realpath "$ort")"
test -r "$ort/include/onnxruntime_cxx_api.h"
test -r "$ort/lib/libonnxruntime.so"
work="$(mktemp -d /tmp/sound-demucs-rt-XXXXXX)"
trap 'rm -rf "$work"' EXIT
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=gnu++17 -O2 -ffunction-sections -fdata-sections \
    -I"$root/src" -I"$inc" -I"$ort/include" "$root/tests/verify-demucs-source-rt-obs.cpp" \
    -Wl,--gc-sections -L"$ort/lib" -lonnxruntime -Wl,-rpath,"$ort/lib" \
    $(pkg-config --libs libobs) -o "$work/check"
"$work/check"
