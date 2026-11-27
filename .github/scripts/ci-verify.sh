#!/usr/bin/env bash
# Source preview: no model downloads, external inference runtimes, or services.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
[[ "${TARGET:?}" == linux-x86_64 && "${OBS_VERSION:?}" == 32.2.0 && "${MODULE:?}" == live-latent-lab-sound && "${PUBLIC_NAME:?}" == "$MODULE" ]]
[[ "${ARCHIVE:?}" == "$MODULE-linux-x86_64-obs32.2.0-test.tar.gz" ]]
[[ "${MINIMUM_TESTS:?}" =~ ^[1-9][0-9]*$ ]]
work="${RUNNER_TEMP:?}/plugin-ci"
srcwork="$RUNNER_TEMP/plugin-ci-source"
mkdir -p "$work" "$srcwork"
sdk="${SOUND_OBS_SDK:-$RUNNER_TEMP/obs-sdk/sdk}"
if [[ -z "${SOUND_OBS_SDK:-}" ]]; then
 bash .github/scripts/prepare-obs32-sdk.sh "$RUNNER_TEMP/obs-sdk"
 test "$(cat "$sdk/OBS_SDK_VERSION")" = "$OBS_VERSION"
fi
test -s "$sdk/lib/cmake/libobs/libobsConfig.cmake"
test -s "$sdk/lib/cmake/obs-frontend-api/obs-frontend-apiConfig.cmake"
export SOUND_OBS_SDK="$sdk"
export PKG_CONFIG_PATH="$sdk/lib/pkgconfig:$sdk/share/pkgconfig:${PKG_CONFIG_PATH:-}"
export LIBRARY_PATH="$sdk/lib:${LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="$sdk/lib:${LD_LIBRARY_PATH:-}"
export CMAKE_PREFIX_PATH="$sdk:${CMAKE_PREFIX_PATH:-}"
export QT_QPA_PLATFORM=offscreen PYTHONDONTWRITEBYTECODE=1
test "$(realpath "$(pkg-config --variable=prefix libobs)")" = "$(realpath "$sdk")"
# A working tree may contain ignored local resources; prove they are not packaged.
canary=$(mktemp "$root/data/.source-preview-canary.XXXXXX")
trap 'rm -f -- "$canary"' EXIT
printf 'SYNTHETIC_NOT_PRIVATE\n' > "$canary"
cmake --preset public-linux -B "$work/build" -DOBS_HEADER_PATH="$sdk/include/obs"
cmake --build "$work/build" --parallel 2
stage="$work/stage"
test ! -e "$stage"
cmake --install "$work/build" --prefix "$stage"
install -m 644 LICENSE "$stage/LICENSE"
install -m 644 NOTICE.md "$stage/NOTICE.md"
test -s "$stage/bin/64bit/$MODULE.so"
# Explicit file allowlist: this archive never ships external runtimes/weights.
printf '%s\n' LICENSE NOTICE.md "bin/64bit/$MODULE.so" data/locale/en-US.ini data/manifest.json | LC_ALL=C sort > "$work/expected-files"
(cd "$stage"; find . -type f -printf '%P\n' | LC_ALL=C sort) > "$work/actual-files"
cmp "$work/expected-files" "$work/actual-files"
test -z "$(find "$stage" -type l -print -quit)"
ldd "$stage/bin/64bit/$MODULE.so" > "$work/ldd.txt"
! grep -Eq 'not found|libonnxruntime|libwhisper|libggml|libcuda|libmediapipe' "$work/ldd.txt"
grep -F "$sdk/lib/" "$work/ldd.txt"
cxx="${CXX:-c++}"
inc="$sdk/include/obs"
"$cxx" -std=gnu++17 -O2 -I"$inc" tests/verify-module-load.cpp -Wl,--no-as-needed -lobs-frontend-api $(pkg-config --libs libobs) -Wl,--as-needed -o "$work/registry"
count=0
xvfb-run -a timeout 30 "$work/registry" "$stage/bin/64bit/$MODULE.so" "$stage/data" yamnet
count=$((count+1))
bash tests/verify-quarantine.sh
count=$((count+1))
"$cxx" -std=c++17 -O2 -pthread -Isrc tests/verify-ring-buffer.cpp -o "$work/ring"
"$work/ring"
count=$((count+1))
"$cxx" -std=c++17 -O2 -Isrc tests/verify-stt-config.cpp -o "$work/stt-config"
"$work/stt-config"
count=$((count+1))
python3 tests/verify-legacy-whisper-rt.py
count=$((count+1))
python3 tests/test_public_repo_urls.py
count=$((count+1))
# Test the optional Qt bridge separately; keep the distributed baseline Qt-free.
"$cxx" -std=c++17 -O2 -fPIC tests/verify-plugin-settings.cpp $(pkg-config --cflags --libs Qt6Widgets) -o "$work/settings-page"
"$work/settings-page"
count=$((count+1))
cmake --preset public-linux -B "$work/settings-build" -DOBS_HEADER_PATH="$sdk/include/obs" -DSOUND_SETTINGS_PAGE=ON
cmake --build "$work/settings-build" --parallel 2
cmake --install "$work/settings-build" --prefix "$work/settings-stage"
"$cxx" -std=gnu++17 -O2 -fPIC -I"$inc" tests/verify-settings-picker.cpp $(pkg-config --cflags --libs Qt6Core) $(pkg-config --libs libobs) -o "$work/settings-picker"
xvfb-run -a timeout 30 "$work/settings-picker" "$work/settings-stage/bin/64bit/$MODULE.so" "$work/settings-stage/data"
count=$((count+1))
(( count >= MINIMUM_TESTS ))
echo "Verified $count independent model-free test groups; no inference qualification implied."
(cd "$stage"; while read -r file; do sha256sum "$file"; done < "$work/expected-files" > "$srcwork/PAYLOAD-SHA256SUMS"; sha256sum -c "$srcwork/PAYLOAD-SHA256SUMS"; tar -czf "$work/$ARCHIVE" -T "$work/expected-files")
(cd "$work"; sha256sum "$ARCHIVE" > SHA256SUMS; sha256sum -c SHA256SUMS)
git diff --quiet HEAD -- .
git archive --format=tar --prefix="$MODULE/" HEAD | gzip -n > "$srcwork/$MODULE-source.tar.gz"
(cd "$srcwork"; sha256sum "$MODULE-source.tar.gz" > SHA256SUMS; sha256sum -c SHA256SUMS)
