#!/usr/bin/env bash
# Build and validate ONE opt-in Live Latent Lab Sound audio filter in a
# separate candidate directory. Never alter a running/user OBS installation.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage="${1:-$HOME/.local/share/live-latent-lab-sound/candidates/mic-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
stage="$(realpath -m "$stage")"
case "$stage" in
  /|/tmp|"$HOME"|"$HOME/.config"/*|/usr/*)
    echo "Refusing an OBS/system/user-profile installation destination: $stage" >&2;exit 2;;
esac
if [[ -e "$stage" || -L "$stage" ]]; then
  echo "Candidate directory already exists; refusing to overwrite: $stage" >&2;exit 2
fi
work="$(mktemp -d /tmp/lll-sound-mic-release-XXXXXX)"
trap 'rm -rf "$work"' EXIT
cmake -S "$root" -B "$work/build" -G Ninja -DFETCH_OBS=OFF \
  -DSOUND_EFFECTS=mic -DSOUND_EXPERIMENTAL=ON -DSOUND_TOOLS_MENU=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_PLUGIN_DESTINATION=bin/64bit -DOBS_DATA_DESTINATION=data \
  -DCMAKE_INSTALL_PREFIX="$stage" >"$work/configure.log"
cmake --build "$work/build" --parallel 4 >"$work/build.log"
nonce="$(cat "$work/build/sound-mic-build-nonce.txt")"
[[ "$nonce" =~ ^[a-f0-9]{16}$ ]] || { echo "Invalid build fingerprint" >&2;exit 1; }
cmake --install "$work/build" >"$work/install.log"
module="$stage/bin/64bit/live-latent-lab-sound.so"
test -s "$module"
if env -u LD_LIBRARY_PATH ldd "$module" | grep -E 'not found|libonnxruntime|libwhisper|libggml|libQt6'; then
  echo "Release candidate has unexpected or missing shared libraries" >&2;exit 1
fi
inc="$(pkg-config --variable=includedir libobs)/obs"
read -r -a libs <<< "$(pkg-config --libs libobs)"
c++ -std=gnu++17 -O2 -I"$inc" "$root/tests/verify-module-load.cpp" \
  -Wl,--no-as-needed -lobs-frontend-api "${libs[@]}" -Wl,--as-needed -o "$work/registry"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
  timeout 25 "$work/registry" "$module" "$stage/data" mic >"$work/registry.log" 2>&1
grep -F "SOUND_REGISTRY_MATCH=1 EXPECTED=[mic] ACTUAL_COUNT=1" "$work/registry.log"
grep -F "SOUND_MIC_DSP_CANDIDATE_$nonce" "$work/registry.log"
c++ -std=gnu++17 -O2 -pthread -I"$inc" \
  "$root/tests/verify-mic-obs-audio.cpp" "${libs[@]}" -o "$work/audio"
QT_QPA_PLATFORM=offscreen xvfb-run -a -s '-screen 0 1280x720x24' \
  timeout 25 "$work/audio" "$module" "$stage/data" >"$work/audio.log" 2>&1
grep -F "pass=1" "$work/audio.log"
sha="$(sha256sum "$module" | cut -d' ' -f1)"
{
  printf 'type=Live Latent Lab Sound microphone DSP (no AI models)\n'
  printf 'nonce=%s\n' "$nonce"
  printf 'marker=SOUND_MIC_DSP_CANDIDATE_%s\n' "$nonce"
  printf 'sha256=%s\n' "$sha"
  printf 'libobs_headers=%s\n' "$(pkg-config --modversion libobs)"
  printf 'runtime_obs=%s\n' "$(obs --version 2>&1 | head -1)"
  printf 'source_git=%s\n' "$(git -C "$root" rev-parse --short HEAD)"
  printf 'registry=SOUND_REGISTRY_MATCH=1 EXPECTED=[mic] ACTUAL_COUNT=1\n'
  grep -F 'MIC_OBS_ACTUAL_AUDIO' "$work/audio.log"
} >"$stage/verified-mic-dsp.txt"
echo "SOUND_MIC_CANDIDATE_PASS=1"
echo "CANDIDATE_DIR=$stage"
echo "BINARY_SHA256=$sha"
echo "OBS remains unmodified. Restart/install is separate and explicit."
