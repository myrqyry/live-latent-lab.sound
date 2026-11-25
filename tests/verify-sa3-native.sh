#!/usr/bin/env bash
# Native local inference qualification; no model downloads, OBS modifications or publication.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_SOURCE:?Set the pinned sa3.cpp checkout path}"
: "${SOUND_SA3_LIBRARY:?Set the staged libsa3.so path}"
: "${SOUND_SA3_MODELS:?Set the local verified GGUF model directory}"
: "${SOUND_SA3_OUTPUT:?Set a new WAV output path}"
test ! -e "$SOUND_SA3_OUTPUT" || { echo "Refusing to overwrite WAV: $SOUND_SA3_OUTPUT" >&2; exit 2; }
test "$(git -C "$SOUND_SA3_SOURCE" rev-parse HEAD)" = 28ebf5f5337ba6f36966db76d16cad1fc5ed2b70
test "$(git -C "$SOUND_SA3_SOURCE/ggml" rev-parse HEAD)" = a373144b892cb1ef7f0c3453efd558caebb0a891
test "$(sha256sum "$SOUND_SA3_SOURCE/src/libsa3_v1.h" | cut -d' ' -f1)" = d4eb236988c978393dac3a7b6470049720dcb3e5dee57a4800d31b5d213342e6
python3 - "$root/tests/fixtures/sa3-small-music-gguf.json" "$SOUND_SA3_MODELS" <<'PY'
import hashlib, json, pathlib, sys
for row in json.loads(pathlib.Path(sys.argv[1]).read_text()):
    path=pathlib.Path(sys.argv[2],row["name"])
    if not path.is_file() or path.stat().st_size != row["size"]:
        raise SystemExit(f"Missing or wrong-size model: {path}")
    with path.open("rb") as f:
        actual=hashlib.file_digest(f,"sha256").hexdigest()
    if actual != row["sha256"]:
        raise SystemExit(f"Wrong SHA256: {path}")
    print(f"MODEL_VERIFIED {path.name}")
PY
if env -u LD_LIBRARY_PATH ldd "$SOUND_SA3_LIBRARY" | grep -E 'not found|/dev/shm/'; then
    echo "Runtime not independently staged" >&2; exit 1
fi
work="$(mktemp -d /tmp/sound-sa3-probe.XXXXXX)"
trap 'rm -rf "$work"' EXIT
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -I"$SOUND_SA3_SOURCE/src" \
    "$root/tests/verify-sa3-native.c" -ldl -lm -o "$work/probe"
env -u LD_LIBRARY_PATH SA3_FLASH_ATTN=0 timeout 120 "$work/probe" \
    "$SOUND_SA3_LIBRARY" "$SOUND_SA3_MODELS" 'gpu:CUDA0' "$SOUND_SA3_OUTPUT" \
    > "$work/run.log" 2>&1 || { cat "$work/run.log"; exit 1; }
cat "$work/run.log"
# The upstream selector may fall back to CPU. Do not infer CUDA from the request.
grep -F '[sa3] backend: CUDA0' "$work/run.log"
grep -Fx 'SA3_NATIVE_PROBE_PASS=1' "$work/run.log"
