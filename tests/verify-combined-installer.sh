#!/usr/bin/env bash
# Exercise install guard/backup with the actual qualified three-effect package.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
candidate="${1:?Pass qualified combined candidate directory}"
installer="$root/tools/install-verified-sound.sh"
work="$(mktemp -d /tmp/lll-combined-install-test-XXXXXX)"
trap 'rm -rf "$work"' EXIT
sha="$(sha256sum "$candidate/bin/64bit/live-latent-lab-sound.so" | cut -d' ' -f1)"
mkdir -p "$work/fake-bin" "$work/home/.config/obs-studio/plugins/live-latent-lab-sound/bin/64bit"
printf 'prior binary' >"$work/home/.config/obs-studio/plugins/live-latent-lab-sound/bin/64bit/live-latent-lab-sound.so"
printf '#!/bin/sh\nexit 0\n' >"$work/fake-bin/pgrep"
chmod +x "$work/fake-bin/pgrep"
run(){ env HOME="$work/home" PATH="$work/fake-bin:$PATH" "$installer" "$@"; }
installed="$work/home/.config/obs-studio/plugins/live-latent-lab-sound/bin/64bit/live-latent-lab-sound.so"
if run "$candidate" --apply >"$work/running.log" 2>&1; then
    echo "Installer accepted running OBS" >&2; exit 1
fi
grep -F 'OBS is running' "$work/running.log"
test "$(cat "$installed")" = 'prior binary'
printf '#!/bin/sh\nexit 1\n' >"$work/fake-bin/pgrep"
run "$candidate" >"$work/dry.log"
test "$(cat "$installed")" = 'prior binary'
if grep -Fxq 'native_options=SOUND_COMBINED_NATIVE_OPTIONS_PASS=1' "$candidate/verified-live-stem.txt"; then
    mkdir "$work/no-workers"
    if XDG_RUNTIME_DIR="$work/no-workers" run "$candidate" --apply >"$work/no-workers.log" 2>&1; then
        echo 'Installer accepted missing native-options workers' >&2; exit 1
    fi
    grep -F 'Native speech options worker unavailable or outdated' "$work/no-workers.log"
    test "$(cat "$installed")" = 'prior binary'
fi
run "$candidate" --apply >"$work/apply.log"
test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
backup="$(sed -n 's/^BACKUP=//p' "$work/apply.log")"
test "$(cat "$backup/bin/64bit/live-latent-lab-sound.so")" = 'prior binary'
# Alter a private COPY of the receipt to verify exact gate refusal.
cp -a "$candidate" "$work/tampered"
sed -i '/^yamnet=/d' "$work/tampered/verified-live-stem.txt"
if run "$work/tampered" --apply >"$work/tampered.log" 2>&1; then
    echo 'Incomplete qualification receipt accepted' >&2; exit 1
fi
test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
if grep -Fxq 'edge_tts=SOUND_COMBINED_EDGE_PASS=1' "$candidate/verified-live-stem.txt"; then
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^edge_tts=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-edge.log" 2>&1; then
        echo 'Incomplete Edge qualification receipt accepted' >&2; exit 1
    fi
    grep -F 'Missing real Edge qualification' "$work/tampered-edge.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'retention=SOUND_COMBINED_RETENTION_PASS=1' "$candidate/verified-live-stem.txt"; then
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^retention=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-retention.log" 2>&1; then
        echo 'Incomplete retention qualification receipt accepted' >&2; exit 1
    fi
    grep -F 'Missing model retention qualification' "$work/tampered-retention.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'long_speech=SOUND_COMBINED_LONG_SPEECH_PASS=1' "$candidate/verified-live-stem.txt"; then
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^long_speech=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-long.log" 2>&1; then
        echo 'Incomplete long speech receipt accepted' >&2; exit 1
    fi
    grep -F 'Missing long speech qualification' "$work/tampered-long.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'native_options=SOUND_COMBINED_NATIVE_OPTIONS_PASS=1' "$candidate/verified-live-stem.txt"; then
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^native_options=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-options.log" 2>&1; then
        echo 'Incomplete native options qualification accepted' >&2; exit 1
    fi
    grep -F 'Missing native speech options qualification' "$work/tampered-options.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'stt=SOUND_COMBINED_STT_PASS=1' "$candidate/verified-live-stem.txt"; then
    for asset in libwhisper.so.1 libggml.so.0 libggml-base.so.0 libggml-cpu.so.0 LICENSE; do
        cmp "$candidate/bin/64bit/stt-runtime/$asset" "$(dirname "$installed")/stt-runtime/$asset"
    done
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^stt=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-stt.log" 2>&1; then
        echo 'Incomplete STT qualification accepted' >&2; exit 1
    fi
    grep -F 'Missing STT qualification' "$work/tampered-stt.log"
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    printf 'invalid runtime' >"$work/tampered/bin/64bit/stt-runtime/libwhisper.so.1"
    if run "$work/tampered" --apply >"$work/tampered-stt-runtime.log" 2>&1; then
        echo 'Modified STT runtime accepted' >&2; exit 1
    fi
    grep -F 'Changed STT runtime asset' "$work/tampered-stt-runtime.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'parakeet=SOUND_COMBINED_PARAKEET_PASS=1' "$candidate/verified-live-stem.txt"; then
    # Prior STT test mutated the copied runtime; restore it first.
    cp -a "$candidate/bin/64bit/stt-runtime/." "$work/tampered/bin/64bit/stt-runtime/"
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^parakeet=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-parakeet.log" 2>&1; then
        echo 'Incomplete Parakeet qualification accepted' >&2; exit 1
    fi
    grep -F 'Missing Parakeet qualification' "$work/tampered-parakeet.log"
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i 's/^parakeet_model_sha256=.*/parakeet_model_sha256=0000000000000000000000000000000000000000000000000000000000000000/' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-parakeet-model.log" 2>&1; then
        echo 'Incorrect Parakeet model hash accepted' >&2; exit 1
    fi
    grep -F 'Missing or changed qualified Parakeet model' "$work/tampered-parakeet-model.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'nemotron=SOUND_COMBINED_NEMOTRON_PASS=1' "$candidate/verified-live-stem.txt"; then
    for asset in libsound-nemotron-runtime.so NEMOTRON-TRANSCRIBE-LICENSE NEMOTRON-GGML-LICENSE NEMOTRON-THIRD-PARTY-LICENSES; do
        cmp "$candidate/bin/64bit/stt-runtime/$asset" "$(dirname "$installed")/stt-runtime/$asset"
    done
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^nemotron=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-nemotron.log" 2>&1; then
        echo 'Incomplete Nemotron qualification accepted' >&2; exit 1
    fi
    grep -F 'Missing Nemotron streaming qualification' "$work/tampered-nemotron.log"
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    printf 'invalid runtime' >"$work/tampered/bin/64bit/stt-runtime/libsound-nemotron-runtime.so"
    if run "$work/tampered" --apply >"$work/tampered-nemotron-runtime.log" 2>&1; then
        echo 'Modified Nemotron runtime accepted' >&2; exit 1
    fi
    grep -F 'Changed Nemotron runtime asset' "$work/tampered-nemotron-runtime.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'sttts=SOUND_COMBINED_STTTS_PASS=1' "$candidate/verified-live-stem.txt"; then
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^sttts=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-sttts.log" 2>&1; then
        echo 'Incomplete speech-to-speech qualification accepted' >&2; exit 1
    fi
    grep -F 'Missing speech-to-speech relay qualification' "$work/tampered-sttts.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
if grep -Fxq 'signals=SOUND_COMBINED_SIGNAL_INTEROP_PASS=1' "$candidate/verified-live-stem.txt"; then
    cp "$candidate/verified-live-stem.txt" "$work/tampered/verified-live-stem.txt"
    sed -i '/^signals=/d' "$work/tampered/verified-live-stem.txt"
    if run "$work/tampered" --apply >"$work/tampered-signals.log" 2>&1; then
        echo 'Incomplete signal qualification accepted' >&2; exit 1
    fi
    grep -F 'Missing cross-plugin signal qualification' "$work/tampered-signals.log"
    test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
fi
echo SOUND_COMBINED_INSTALLER_SAFE_PASS=1
