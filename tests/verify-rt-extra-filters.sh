#!/usr/bin/env bash
# Linux/glibc scoped callback instrumentation. Local assets only.
# Optional stem test owns a private worker, never the active OBS socket.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_RT_STAGE:?staged Sound build containing yamnet and stem_live}"
: "${SOUND_MEDIAPIPE_LIB:?qualified MediaPipe Tasks native runtime}"
: "${SOUND_YAMNET_MODEL:?qualified YAMNet model}"
work="$(mktemp -d /tmp/sound-rt-extra.XXXXXX)"
worker=""
cleanup(){
    if [[ -n "$worker" ]]; then
        kill "$worker" 2>/dev/null || true
        wait "$worker" 2>/dev/null || true
    fi
    if [[ -f "$work/sidecar.log" ]]; then
        grep -E 'STEM_QUALITY|STEM_WARMUP|STEM_WINDOW' "$work/sidecar.log" || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT
inc="$(pkg-config --variable=includedir libobs)/obs"
read -r -a libs <<< "$(pkg-config --libs libobs)"
module="$SOUND_RT_STAGE/bin/64bit/live-latent-lab-sound.so"
data="$SOUND_RT_STAGE/data"
"${CC:-cc}" -std=gnu11 -O2 -fno-builtin -fPIC -shared -pthread \
    "$root/tests/rt-probe-fixture.c" -o "$work/fixture.so"
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -rdynamic -I"$inc" \
    "$root/tests/verify-rt-probe.cpp" "${libs[@]}" -ldl -o "$work/probe"
"$work/probe" "$work/fixture.so"
for t in yamnet-obs-audio stem-live-obs-audio; do
    "${CXX:-c++}" -std=gnu++17 -O2 -pthread -rdynamic -DSOUND_RT_PROBE -I"$inc" \
        "$root/tests/verify-$t.cpp" "${libs[@]}" -ldl -o "$work/$t"
done
for rate in 48000 44100; do
    SOUND_YAMNET_DEVICE=cpu SOUND_YAMNET_TEST_RATE="$rate" QT_QPA_PLATFORM=offscreen \
      xvfb-run -a timeout 35 "$work/yamnet-obs-audio" "$module" "$data" "$SOUND_MEDIAPIPE_LIB" "$SOUND_YAMNET_MODEL"
done
if [[ "${1:-}" == --with-stems ]]; then
    : "${STEM_SPLITTER_PYTHON:?local stem worker Python}"
    : "${SOUND_RT_MUSIC:?24 seconds or more 48 kHz interleaved stereo float32 music}"
    test "$(stat -c %s "$SOUND_RT_MUSIC")" -ge 9216000
    HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 "$STEM_SPLITTER_PYTHON" "$root/tools/live_stem_sidecar.py" \
        --device cuda --shifts 0 --overlap .1 --segment-seconds 4 --precision fp32 \
        --socket "$work/model.sock" > "$work/sidecar.log" 2>&1 &
    worker=$!
    for ((i=0;i<180;i++)); do
        [[ -S "$work/model.sock" ]] && break
        kill -0 "$worker" || { cat "$work/sidecar.log"; exit 1; }
        sleep .2
    done
    test -S "$work/model.sock" || { cat "$work/sidecar.log"; exit 1; }
    QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 42 "$work/stem-live-obs-audio" \
        "$module" "$data" "$SOUND_RT_MUSIC" "$work/model.sock"
    QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 22 "$work/stem-live-obs-audio" \
        "$module" "$data" "$SOUND_RT_MUSIC" "$work/missing.sock" --expect-silence
elif [[ -n "${1:-}" ]]; then
    echo "Usage: $0 [--with-stems]" >&2
    exit 2
fi
