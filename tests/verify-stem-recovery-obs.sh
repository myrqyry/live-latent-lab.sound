#!/usr/bin/env bash
# Deterministic OBS transport/lifecycle test. No real separation or GPU claim.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_STEM_STAGE:?Stage containing the opt-in stem module}"
work="$(mktemp -d /tmp/stem-recovery-obs.XXXXXX)"
pid=""
cleanup(){
    if [[ -n "$pid" ]];then kill "$pid" 2>/dev/null || true;wait "$pid" 2>/dev/null || true;fi
    if [[ -d "$work" ]];then rm -rf -- "$work";fi
}
trap cleanup EXIT
fixture="${SOUND_STEM_FIXTURE_RAW:-$work/fixture.raw}"
if [[ ! -f "$fixture" && -z "${SOUND_STEM_FIXTURE_RAW:-}" ]];then
    python3 - "$fixture" <<'PY'
import array, math, sys
pcm=array.array("f")
for i in range(48000):
    pcm.extend((.2*math.sin(2*math.pi*220*i/48000),.1*math.sin(2*math.pi*330*i/48000)))
if sys.byteorder!="little":pcm.byteswap()
with open(sys.argv[1],"wb") as f:
    for _ in range(24):pcm.tofile(f)
PY
fi
test -s "$fixture"
inc="$(pkg-config --variable=includedir libobs)/obs"
probe=()
if [[ "${SOUND_RT_PROBE:-0}" == 1 ]];then probe=(-DSOUND_RT_PROBE -rdynamic -ldl);fi
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-stem-live-obs-audio.cpp" $(pkg-config --libs libobs) "${probe[@]}" -o "$work/obs"
for mode in AUTO_RECOVER RECOVER;do
    endpoint="$work/$mode.sock"
    python3 "$root/tests/stem-protocol-fixture.py" "$endpoint" >"$work/$mode-peer.log" 2>&1 &
    pid=$!
    for ((i=0;i<100;i++));do
        [[ -S "$endpoint" ]] && break
        kill -0 "$pid";sleep .05
    done
    [[ -S "$endpoint" ]]
    env "SOUND_STEM_$mode"=1 QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 80 "$work/obs" "$SOUND_STEM_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_STEM_STAGE/data" "$fixture" "$endpoint" >"$work/$mode.log" 2>&1 || { tail -60 "$work/$mode.log";exit 1; }
    grep "STEM_LIVE_$mode.*pass=1" "$work/$mode.log"
    grep '^RT_CALLBACK ' "$work/$mode.log" || true
    kill "$pid";wait "$pid" 2>/dev/null || true;pid=""
done
# Four independent OBS sources, exact fixture stereo values and shared timestamps.
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-stem-output-obs.cpp" $(pkg-config --libs libobs) "${probe[@]}" -o "$work/outputs"
endpoint="$work/outputs.sock"
python3 "$root/tests/stem-protocol-fixture.py" "$endpoint" >"$work/outputs-peer.log" 2>&1 &
pid=$!
for ((i=0;i<100;i++));do
    [[ -S "$endpoint" ]] && break
    kill -0 "$pid";sleep .05
done
[[ -S "$endpoint" ]]
SOUND_STEM_OUTPUT_FIXTURE=1 QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 45 "$work/outputs" "$SOUND_STEM_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_STEM_STAGE/data" "$fixture" "$endpoint" >"$work/outputs.log" 2>&1 || { tail -60 "$work/outputs.log";exit 1; }
grep -E '^STEM_OUTPUT |^STEM_FANOUT_OBS |^RT_CALLBACK ' "$work/outputs.log"
echo SOUND_STEM_RECOVERY_OBS_PROTOCOL_ONLY_PASS=1
