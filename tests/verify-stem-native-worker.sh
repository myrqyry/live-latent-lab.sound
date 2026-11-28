#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${SOUND_STEM_NATIVE_HELPER:?Set native C++ helper executable}"
: "${SOUND_STEM_NATIVE_MODEL:?Set local GGUF}"
work="$(mktemp -d /tmp/lll-native-stem-test-XXXXXX)"
pid=""
cleanup(){
    if [[ -n "$pid" ]];then kill "$pid" 2>/dev/null || true;wait "$pid" 2>/dev/null || true;fi
    echo "Native worker evidence: $work"
}
trap cleanup EXIT
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$root/src" "$root/tests/verify-stem-v2-client.cpp" -o "$work/client"
"$SOUND_STEM_NATIVE_HELPER" --model "$SOUND_STEM_NATIVE_MODEL" --socket "$work/native.sock" --parent-pid "$$" >"$work/model.log" 2>&1 &
pid=$!
for ((i=0;i<300;i++));do
    [[ -S "$work/native.sock" ]] && break
    kill -0 "$pid" || { tail -30 "$work/model.log";exit 1; }
    sleep .1
done
test -S "$work/native.sock"
"$work/client" "$work/native.sock"
python3 - "$work/native.sock" <<'PY'
import socket,struct,sys
path=sys.argv[1]
h=struct.Struct("<8s8I")
for bad in ("profile","nonfinite"):
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
        s.settimeout(5);s.connect(path)
        s.sendall(h.pack(b"LLSTEM02",2,44100,123480,2,72,0,0,99 if bad=="profile" else 1))
        if bad=="nonfinite":s.sendall(struct.pack("<f",float("nan"))+bytes((2*123480-1)*4))
        raw=bytearray()
        while len(raw)<40:
            part=s.recv(40-len(raw));assert part;raw.extend(part)
        reply=h.unpack(raw)
        assert reply[0]==b"LLSTEM02" and reply[6:]==(0,1,1)
        assert s.recv(1)==b""
print("NATIVE_WORKER_REJECTION_PASS=1")
PY
"$work/client" "$work/native.sock"
grep -E '^STEM_NATIVE_' "$work/model.log"
kill "$pid";wait "$pid" || true;pid=""
echo NATIVE_WORKER_CONTRACT_PASS=1
