#!/usr/bin/env bash
# Real-model local acceptance; does not touch the active OBS profile.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_STAGE:?Set the staged Sound directory}"
: "${SOUND_SA3_SOURCE:?Set the pinned upstream sa3.cpp checkout}"
: "${SOUND_SA3_MODELS:?Set the local verified model directory}"
: "${SOUND_SA3_INPUT_WAV:?Set an existing music WAV of at least two seconds}"
test "$(git -C "$SOUND_SA3_SOURCE" rev-parse HEAD)" = 28ebf5f5337ba6f36966db76d16cad1fc5ed2b70
test "$(git -C "$SOUND_SA3_SOURCE/ggml" rev-parse HEAD)" = a373144b892cb1ef7f0c3453efd558caebb0a891
runtime="$SOUND_SA3_STAGE/bin/64bit/sa3-runtime"
(cd "$runtime"; sha256sum -c SHA256SUMS)
python3 - "$root" "$SOUND_SA3_MODELS" <<'PY'
import hashlib,json,pathlib,sys
root,models=map(pathlib.Path,sys.argv[1:])
for manifest in ('sa3-small-music-gguf.json','sa3-small-sfx-gguf.json'):
    for row in json.loads((root/'tests/fixtures'/manifest).read_text()):
        path=models/row['name']
        if not path.is_file() or path.stat().st_size!=row['size']:
            raise SystemExit(f'Missing/wrong size: {path}')
        with path.open('rb') as f:
            if hashlib.file_digest(f,'sha256').hexdigest()!=row['sha256']:
                raise SystemExit(f'Wrong hash: {path}')
        print('MODEL_VERIFIED',path.name)
PY
work="$(mktemp -d /tmp/sound-sa3-upgrades.XXXXXX)"
trap 'rm -rf "$work"' EXIT
module="$SOUND_SA3_STAGE/bin/64bit/live-latent-lab-sound.so"
data="$SOUND_SA3_STAGE/data"
base="$(find "$runtime" -type f -name 'libsa3-*-ggml-base.so.0.29.0' -print -quit)"
test -n "$base"
"${CXX:-c++}" -std=c++17 -O2 -I"$SOUND_SA3_SOURCE/ggml/include" \
    "$root/tests/make-sa3-test-adapter.cpp" "$base" -Wl,-rpath,"$runtime" -o "$work/make-adapter"
"$work/make-adapter" "$SOUND_SA3_MODELS/stable-audio-3-small-music-dit-0.5B-v1.0-Q4_K_M.gguf" "$work/test-only.gguf"
ffmpeg -nostdin -v error -i "$SOUND_SA3_INPUT_WAV" -t 2 -ar 44100 -ac 2 -c:a pcm_s16le "$work/input.wav"
inc="$(pkg-config --variable=includedir libobs)/obs"
for test in verify-sa3-continuous verify-sa3-operations verify-sa3-status-refresh; do
    "${CXX:-c++}" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/$test.cpp" $(pkg-config --libs libobs) -o "$work/$test"
done
export QT_QPA_PLATFORM=offscreen
xvfb-run -a timeout 15 "$work/verify-sa3-status-refresh" "$module" "$data" >"$work/refresh.log" 2>&1 || { cat "$work/refresh.log"; exit 1; }
grep -F 'SA3_STATUS_REFRESH clicked=1 changed=1 pass=1' "$work/refresh.log"
xvfb-run -a timeout 110 "$work/verify-sa3-operations" "$module" "$data" "$SOUND_SA3_MODELS" "$work/input.wav" "$work/test-only.gguf" >"$work/ops.log" 2>&1 || { cat "$work/ops.log"; exit 1; }
grep -F '[sa3] backend: CUDA0' "$work/ops.log"
grep -F 'lora lanes: 1 lane(s)' "$work/ops.log"
grep -F 'SA3_SIGHT_FIXTURE fresh=1 stale_fallback=1' "$work/ops.log"
grep -F 'SA3_ADVANCED capture=1 captured=1 automation=1 invalid_adapter=1 pass=1' "$work/ops.log"
grep -F 'SA3_OPERATIONS sfx=1 transform=1 extend=1 invalid_wav=1 cap=1 pass=1' "$work/ops.log"
for rate in 48000 44100; do
    args=()
    if [[ "$rate" == 44100 ]]; then args=(SOUND_DIFFUSION_TEST_44100=1); fi
    env -u SOUND_DIFFUSION_TEST_44100 "${args[@]}" xvfb-run -a timeout 55 "$work/verify-sa3-continuous" "$module" "$data" "$SOUND_SA3_MODELS" >"$work/$rate.log" 2>&1 || { cat "$work/$rate.log"; exit 1; }
    grep -F '[sa3] backend: CUDA0' "$work/$rate.log"
    grep -F "SA3_CONTINUOUS rate=$rate " "$work/$rate.log" | grep -F 'pass=1'
done
"$root/tests/verify-sa3-obs.sh"
