#!/usr/bin/env bash
# Compiles the real filter callback against local OBS, Eigen and ONNX headers.
# The synthetic stem fixture qualifies only the audio transport.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="${1:?usage: verify-demucs-filter-rt-obs.sh /path/to/build/_deps}"
deps="$(realpath "$deps")"
ort="$deps/onnxruntime-src"
test -r "$ort/include/onnxruntime_cxx_api.h"
test -r "$deps/demucs_onnx-src/src/demucs.hpp"
test -d "$deps/eigen-src/Eigen"
work="$(mktemp -d /tmp/sound-demucs-filter-rt-XXXXXX)"
trap 'rm -rf "$work"' EXIT
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=gnu++17 -O2 -ffunction-sections -fdata-sections \
    -I"$root/src" -I"$root/src/onnx_shims" -I"$inc" -I"$ort/include" \
    -I"$deps/demucs_onnx-src/src" -I"$deps/eigen-src" \
    "$root/tests/verify-demucs-filter-rt-obs.cpp" -Wl,--gc-sections \
    -L"$ort/lib" -lonnxruntime -Wl,-rpath,"$ort/lib" \
    $(pkg-config --libs libobs) -o "$work/check"
"$work/check"
