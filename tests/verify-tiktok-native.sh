#!/usr/bin/env bash
# Offline codec/contract check. FFmpeg CLI is a test-fixture generator only.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cd "$root"
ffmpeg -nostdin -v error -f lavfi -i 'sine=frequency=660:sample_rate=24000:duration=1' -ac 1 -c:a libmp3lame "$work/fixture.mp3"
python3 - "$work" <<'PY'
import base64,json,pathlib,sys
p=pathlib.Path(sys.argv[1])
(p/'response.json').write_text(json.dumps({'success':True,'data':base64.b64encode((p/'fixture.mp3').read_bytes()).decode('ascii')}))
PY
read -ra libs <<< "$(pkg-config --libs libobs libavformat libavcodec libavutil libswresample)"
read -ra flags <<< "$(pkg-config --cflags libavformat libavcodec libavutil libswresample)"
"${CXX:-c++}" -std=c++17 -O2 -I src -I"$(pkg-config --variable=includedir libobs)/obs" "${flags[@]}" tests/verify-tiktok-native.cpp "${libs[@]}" -o "$work/test"
QT_QPA_PLATFORM=offscreen xvfb-run -a "$work/test" "$work/response.json"
