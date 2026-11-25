#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_SA3_STAGE:?Set staged Sound directory}"
: "${SOUND_SA3_MODELS:?Set qualified model directory}"
: "${SOUND_SAME_INPUT_A:?Set two-second stereo 44.1 kHz PCM16 WAV}"
: "${SOUND_SAME_INPUT_B:?Set different two-second WAV}"
work="$(mktemp -d /tmp/sound-same-texture.XXXXXX)"
trap 'rm -rf "$work"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -I"$root/src" "$root/tests/verify-same-texture.cpp" -o "$work/policy"
"$work/policy"
helper="$SOUND_SA3_STAGE/bin/64bit/sa3-runtime/sound-sa3-morph"
model="$SOUND_SA3_MODELS/stable-audio-3-small-music-same-s-v1.0-F16.gguf"
for mode in freeze smear grain; do
    timeout 30 "$helper" "$model" "$SOUND_SAME_INPUT_A" "" .8 "$work/$mode.wav" gpu:CUDA0 "$mode" .4 3 4
done
timeout 30 "$helper" "$model" "$SOUND_SAME_INPUT_A" "" 0 "$work/zero.wav" gpu:CUDA0 freeze .4 3 4
timeout 30 "$helper" "$model" "$SOUND_SAME_INPUT_A" "$SOUND_SAME_INPUT_B" 0 "$work/bypass.wav" gpu:CUDA0 fixed
timeout 30 "$helper" "$model" "$SOUND_SAME_INPUT_A" "" .8 "$work/cpu.wav" cpu smear .4 3 4
if "$helper" "$model" "$SOUND_SAME_INPUT_A" "" .8 "$work/invalid.wav" cpu grain .4 3 0;then exit 1;fi
test ! -e "$work/invalid.wav"
python3 - "$work" <<'PY'
import array, math, pathlib, sys, wave
w=pathlib.Path(sys.argv[1])
def read(n):
    with wave.open(str(w/(n+'.wav')),'rb') as f:
        assert (f.getnchannels(),f.getframerate(),f.getsampwidth(),f.getnframes())==(2,44100,2,88200)
        a=array.array('h',f.readframes(f.getnframes()))
        if sys.byteorder!='little':a.byteswap()
        assert 100<max(map(abs,a))<=24577
        return a
def diff(a,b):return math.sqrt(math.fsum(((x-y)/32768)**2 for x,y in zip(a,b))/len(a))
base=read('bypass')
assert read('zero')==base
modes=[read(m) for m in ('freeze','smear','grain')]
for i,v in enumerate(modes):
    d=diff(v,base);print('SAME_TEXTURE_AUDIO',i,d);assert d>.005
    for u in modes[:i]:assert diff(v,u)>.005
parity=diff(read('cpu'),modes[1]);print('SAME_TEXTURE_CPU_GPU_RMS',parity);assert parity<.02
print('SAME_TEXTURE_AUDIO_PASS=1')
PY
SOUND_SAME_TEXTURE=1 "$root/tests/verify-sa3-morph.sh"
