#!/usr/bin/env bash
# Real codec and OBS worker-path acceptance; inputs must be different,
# stereo 44.1 kHz PCM16 WAVs of exactly two seconds.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_STAGE:?Set morph-enabled staged Sound plugin directory}"
: "${SOUND_SA3_MODELS:?Set local verified SA3 model directory}"
: "${SOUND_SAME_INPUT_A:?Set first WAV fixture}"
: "${SOUND_SAME_INPUT_B:?Set second WAV fixture}"
work="$(mktemp -d /tmp/sound-same-test.XXXXXX)"
trap 'rm -rf "$work"' EXIT
runtime="$SOUND_SA3_STAGE/bin/64bit/sa3-runtime"
model="$SOUND_SA3_MODELS/stable-audio-3-small-music-same-s-v1.0-F16.gguf"
helper="$runtime/sound-sa3-morph"
(cd "$runtime"; sha256sum -c SHA256SUMS)
for mix in 0 .5 1; do
    timeout 30 "$helper" "$model" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" "$mix" "$work/mix-$mix.wav" gpu:CUDA0 fixed
done
timeout 30 "$helper" "$model" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" .5 "$work/cpu.wav" cpu fixed
timeout 30 "$helper" "$model" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" 1 "$work/ramp.wav" gpu:CUDA0 ramp
if "$helper" "$model" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" .5 "$work/mix-0.wav" cpu fixed;then exit 1;fi
if "$helper" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" .5 "$work/invalid.wav" cpu fixed;then exit 1;fi
test ! -e "$work/invalid.wav"
python3 - "$work" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" <<'PY'
import array, math, pathlib, sys, wave
w=pathlib.Path(sys.argv[1])
def read(p):
    with wave.open(str(p),'rb') as f:
        assert (f.getnchannels(),f.getframerate(),f.getsampwidth(),f.getnframes())==(2,44100,2,88200)
        a=array.array('h',f.readframes(f.getnframes()))
        if sys.byteorder!='little':a.byteswap()
        return [v/32768 for v in a]
def rms(x):
    return math.sqrt(math.fsum(v*v for v in x)/len(x))
def corr(x,y):
    mx=math.fsum(x)/len(x);my=math.fsum(y)/len(y)
    a=[v-mx for v in x];b=[v-my for v in y]
    return math.fsum(v*u for v,u in zip(a,b))/math.sqrt(math.fsum(v*v for v in a)*math.fsum(u*u for u in b))
a,b=map(read,sys.argv[2:])
x,z,y,cpu,ramp=[read(w/n) for n in ('mix-0.wav','mix-.5.wav','mix-1.wav','cpu.wav','ramp.wav')]
for v in (x,y,z,cpu,ramp):
    assert .0001<rms(v)<.75 and max(map(abs,v))<=.751
ca,cb,cc=corr(a,x),corr(b,y),corr(z,cpu)
difference=rms([v-(u+t)/2 for v,u,t in zip(z,x,y)])
ramp_difference=rms([v-u for v,u in zip(ramp,z)])
print(f"SAME_AUDIO_VALIDATION A={ca:.6f} B={cb:.6f} CPU_GPU={cc:.6f} CROSSFADE_RMS={difference:.6f} RAMP_RMS={ramp_difference:.6f}")
assert ca>.5 and cb>.5 and cc>.9 and difference>.005 and ramp_difference>.005
PY
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$inc" "$root/tests/verify-sa3-morph-obs.cpp" $(pkg-config --libs libobs) -o "$work/probe"
QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 45 "$work/probe" "$SOUND_SA3_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_SA3_STAGE/data" "$SOUND_SA3_MODELS" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" >"$work/obs.log" 2>&1 || { cat "$work/obs.log";exit 1; }
grep -F 'SA3_MORPH_CANCEL cancelled=1 retry=1' "$work/obs.log"
grep -F 'SA3_MORPH_OBS available=1 generated=1 audio=1 replay=1 missing=1 recovered=1 pass=1' "$work/obs.log"
