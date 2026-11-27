#!/usr/bin/env bash
# Requires local ONNX Runtime SDK and Python onnx. No model download or user install.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ort="$(realpath "${1:?usage: verify-deepfilter-obs.sh /path/to/onnxruntime-src}")"
test -r "$ort/include/onnxruntime_cxx_api.h"
test -r "$ort/lib/libonnxruntime.so"
work="$(mktemp -d /tmp/sound-df-obs-XXXXXX)"
trap 'rm -rf "$work"' EXIT
cd "$root"
cmake -S . -B "$work/build" -G Ninja -DFETCH_OBS=OFF -DSOUND_EXPERIMENTAL=ON \
    -DSOUND_EFFECTS=deepfilter -DSOUND_TOOLS_MENU=OFF \
    -DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME="$ort" \
    -DOBS_PLUGIN_DESTINATION=bin/64bit -DOBS_DATA_DESTINATION=data -DCMAKE_BUILD_TYPE=Release
cmake --build "$work/build" --parallel 2
cmake --install "$work/build" --prefix "$work/stage"
python3 - "$work" <<'PY'
import sys
from pathlib import Path
from onnx import helper, TensorProto, save, checker
for name,gain in [('quarter',.25),('three_quarters',.75)]:
    x=helper.make_tensor_value_info('audio',TensorProto.FLOAT,[1,1,480])
    y=helper.make_tensor_value_info('output',TensorProto.FLOAT,[1,1,480])
    scale=helper.make_tensor('gain',TensorProto.FLOAT,[1],[gain])
    graph=helper.make_graph([helper.make_node('Mul',['audio','gain'],['output'])],
                            'transport_fixture',[x],[y],[scale])
    model=helper.make_model(graph,opset_imports=[helper.make_opsetid('',13)])
    model.ir_version=8
    checker.check_model(model)
    save(model,str(Path(sys.argv[1])/(name+'.onnx')))
PY
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=gnu++17 -O2 -I"$inc" tests/verify-deepfilter-obs.cpp \
    -Wl,--no-as-needed -lobs-frontend-api $(pkg-config --libs libobs) \
    -Wl,--as-needed -o "$work/check"
QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 30 "$work/check" \
    "$work/stage/bin/64bit/live-latent-lab-sound.so" "$work/stage/data" \
    "$work/quarter.onnx" "$work/three_quarters.onnx"
