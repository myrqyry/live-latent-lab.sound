#!/usr/bin/env bash
# Isolated real reader + OBS acceptance. Does not modify the installed service.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_KOKORO_LIBRARY:?Set qualified audio.cpp C ABI library}"
: "${SOUND_KOKORO_MODEL:?Set pinned Q8 GGUF model}"
: "${SOUND_READER_PYTHON:?Set existing reader interpreter}"
: "${SOUND_READER_ASSETS:?Set existing ONNX reader assets}"
: "${SOUND_STAGE:?Set staged Sound plugin}"
work="$(mktemp -d /tmp/sound-native-tts-test.XXXXXX)"
pid=""
cleanup(){ if [[ -n "$pid" ]];then kill "$pid" 2>/dev/null||true;wait "$pid" 2>/dev/null||true;fi;rm -rf "$work"; }
trap cleanup EXIT
mkdir -m700 "$work/lll-sound-tts"
export XDG_RUNTIME_DIR="$work"
"$SOUND_READER_PYTHON" "$root/tests/verify-native-kokoro.py" "$SOUND_KOKORO_LIBRARY" "$SOUND_KOKORO_MODEL"
"$SOUND_READER_PYTHON" "$root/tools/tts_worker.py" --mode reader --assets "$SOUND_READER_ASSETS" --socket "$work/lll-sound-tts/speech.sock" --device cpu --native-kokoro-library "$SOUND_KOKORO_LIBRARY" --native-kokoro-model "$SOUND_KOKORO_MODEL" >"$work/server.log" 2>&1 & pid=$!
for i in $(seq 1 100);do [[ -S "$work/lll-sound-tts/speech.sock" ]]&&break;kill -0 "$pid";sleep .1;done
test -S "$work/lll-sound-tts/speech.sock"
inc="$(pkg-config --variable=includedir libobs)/obs"
"${CXX:-c++}" -std=gnu++17 -O2 -pthread -I"$inc" "$root/tests/verify-tts-obs.cpp" $(pkg-config --libs libobs) -o "$work/probe"
for backend in kokoro_native kokoro piper;do
  voice=af_heart
  [[ "$backend" == piper ]]&&voice=en_US-amy-low
  QT_QPA_PLATFORM=offscreen xvfb-run -a timeout 80 "$work/probe" "$SOUND_STAGE/bin/64bit/live-latent-lab-sound.so" "$SOUND_STAGE/data" "$backend" "$voice" >"$work/$backend.log" 2>&1 || { cat "$work/$backend.log" "$work/server.log";exit 1; }
  grep -F "SOUND_TTS_REAL_OBS backend=$backend" "$work/$backend.log" | grep -F 'pass=1'
  if [[ "$backend" == kokoro_native ]];then grep -F 'SOUND_NATIVE_TTS speed=1 cancel=1 recovered=1' "$work/$backend.log";fi
done
# Explicit native lease eviction and failed option handling through the real socket.
"$SOUND_READER_PYTHON" - "$work/lll-sound-tts/speech.sock" <<'PY'
import http.client,socket,json,sys,time
class Local(http.client.HTTPConnection):
    def connect(self):
        self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.sock.settimeout(45);self.sock.connect(sys.argv[1])
def req(path,body=None):
    c=Local('localhost')
    c.request('GET' if body is None else 'POST',path,None if body is None else json.dumps(body),{'Content-Type':'application/json'})
    r=c.getresponse();data=r.read();status=r.status;c.close();return status,data
lease=dict(model='kokoro_native',client='verification',action='acquire',ttl=0)
assert req('/v1/models/lease',lease)[0]==200
speech=dict(model='kokoro_native',voice='af_heart',input='Retention test.',response_format='wav',retention_ttl=0)
assert req('/v1/audio/speech',speech)[0]==200
assert 'kokoro_native' in json.loads(req('/v1/models/status')[1])['loaded']
lease['action']='release';assert req('/v1/models/lease',lease)[0]==200
for _ in range(30):
    if 'kokoro_native' not in json.loads(req('/v1/models/status')[1])['loaded']:break
    time.sleep(.1)
else:raise AssertionError('Native worker was not evicted')
speech['options']={'is_phonemes':True}
assert req('/v1/audio/speech',speech)[0]==400
print('SOUND_NATIVE_TTS_SOCKET_RETENTION=1')
PY
