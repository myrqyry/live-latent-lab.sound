#!/usr/bin/env bash
# Intentional install of an already tested Sound candidate; not an updater.
# It never stops OBS, installs system-wide, or removes other plugin files.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 CANDIDATE_DIR [--apply]" >&2;exit 2
fi
candidate="$(realpath -m "$1")"
apply="${2:-}"
[[ -z "$apply" || "$apply" == '--apply' ]] || { echo 'Only --apply is supported' >&2;exit 2; }
source="$candidate/bin/64bit/live-latent-lab-sound.so"
receipt="$candidate/verified-live-stem.txt"
[[ -f "$source" && -f "$receipt" && ! -L "$source" ]] ||
  { echo 'Missing verified candidate binary/receipt' >&2;exit 2; }
sha="$(sha256sum "$source" | cut -d' ' -f1)"
grep -Fxq "sha256=$sha" "$receipt" ||
  { echo 'Candidate SHA256 does not match its verification receipt' >&2;exit 2; }
marker="$(sed -n 's/^marker=//p' "$receipt")"
[[ "$marker" =~ ^SOUND_STEM_LIVE_CANDIDATE_[a-f0-9]{16}$ ]] ||
  { echo 'Invalid or missing combined candidate marker' >&2;exit 2; }
effects="$(sed -n 's/^effects=//p' "$receipt")"
native_options_required=false
stt_required=false
nemotron_assets=()
case "$effects" in
    'stem_live;yamnet') count=2 ;;
    'stem_live;yamnet;tts'|'stem_live;yamnet;tts;stt')
        count=3
        grep -Fxq 'unified_tts=SOUND_TTS_UNIFIED_PASS=1' "$receipt" ||
          { echo 'Missing unified TTS qualification' >&2;exit 2; }
        if strings "$source" | grep -Fx -e 'SOUND_TTS_EDGE_20261118_E521' -e '[Sound/TTS] SOUND_TTS_EDGE_20261118_E521' >/dev/null; then
            grep -Fxq 'edge_tts=SOUND_COMBINED_EDGE_PASS=1' "$receipt" ||
              { echo 'Missing real Edge qualification' >&2;exit 2; }
            tools="$(cd "$(dirname "$0")" && pwd)"
            for asset in edge_tts_worker.py edge-voices.json edge-requirements.txt tts-units/lll-sound-edge.service; do
                grep -Fxq "edge_asset:$asset=$(sha256sum "$tools/$asset" | cut -d' ' -f1)" "$receipt" ||
                  { echo "Changed Edge asset: $asset" >&2;exit 2; }
            done
        fi
        if strings "$source" | grep -Fx -e 'SOUND_TTS_RETENTION_20261118_R621' -e '[Sound/TTS] SOUND_TTS_RETENTION_20261118_R621' >/dev/null; then
            grep -Fxq 'retention=SOUND_COMBINED_RETENTION_PASS=1' "$receipt" ||
              { echo 'Missing model retention qualification' >&2;exit 2; }
        fi
        if strings "$source" | grep -Fx -e 'SOUND_TTS_LONG_INPUT_20261118_L673' -e '[Sound/TTS] SOUND_TTS_LONG_INPUT_20261118_L673' >/dev/null; then
            grep -Fxq 'long_speech=SOUND_COMBINED_LONG_SPEECH_PASS=1' "$receipt" ||
              { echo 'Missing long speech qualification' >&2;exit 2; }
        fi
        if strings "$source" | grep -Fx -e 'SOUND_TTS_NATIVE_OPTIONS_20261118_O536' -e '[Sound/TTS] SOUND_TTS_NATIVE_OPTIONS_20261118_O536' >/dev/null; then
            grep -Fxq 'native_options=SOUND_COMBINED_NATIVE_OPTIONS_PASS=1' "$receipt" ||
              { echo 'Missing native speech options qualification' >&2;exit 2; }
            native_options_required=true
        fi
        if [[ "$effects" == 'stem_live;yamnet;tts;stt' ]]; then
            stt_required=true
            if strings "$source" | grep -Fx -e 'SOUND_SIGNAL_INTEROP_20261120_I421' -e '[Sound/Signals] SOUND_SIGNAL_INTEROP_20261120_I421' >/dev/null; then
                grep -Fxq 'signals=SOUND_COMBINED_SIGNAL_INTEROP_PASS=1' "$receipt" ||
                  { echo 'Missing cross-plugin signal qualification' >&2;exit 2; }
            fi
            if strings "$source" | grep -Fx -e 'SOUND_STTTS_RELAY_20261120_R820' -e '[Sound/TTS] SOUND_STTTS_RELAY_20261120_R820' >/dev/null; then
                grep -Fxq 'sttts=SOUND_COMBINED_STTTS_PASS=1' "$receipt" ||
                  { echo 'Missing speech-to-speech relay qualification' >&2;exit 2; }
            fi
            count=4
            grep -Fxq 'stt=SOUND_COMBINED_STT_PASS=1' "$receipt" ||
              { echo 'Missing STT qualification' >&2;exit 2; }
            strings "$source" | grep -Fx 'SOUND_STT_FILTER_20261118_S813' >/dev/null ||
              { echo 'Missing STT binary marker' >&2;exit 2; }
            if strings "$source" | grep -Fx 'SOUND_STT_PARAKEET_20261119_P614' >/dev/null; then
                grep -Fxq 'parakeet=SOUND_COMBINED_PARAKEET_PASS=1' "$receipt" ||
                  { echo 'Missing Parakeet qualification' >&2;exit 2; }
                parakeet_model="$(sed -n 's/^parakeet_model=//p' "$receipt")"
                [[ -f "$parakeet_model" ]] && grep -Fxq "parakeet_model_sha256=$(sha256sum "$parakeet_model" | cut -d' ' -f1)" "$receipt" ||
                  { echo 'Missing or changed qualified Parakeet model' >&2;exit 2; }
            fi
            if strings "$source" | grep -Fx 'SOUND_STT_NEMOTRON_20261119_N560' >/dev/null; then
                grep -Fxq 'nemotron=SOUND_COMBINED_NEMOTRON_PASS=1' "$receipt" ||
                  { echo 'Missing Nemotron streaming qualification' >&2;exit 2; }
                nemotron_model="$(sed -n 's/^nemotron_model=//p' "$receipt")"
                [[ -f "$nemotron_model" ]] && grep -Fxq "nemotron_model_sha256=$(sha256sum "$nemotron_model" | cut -d' ' -f1)" "$receipt" ||
                  { echo 'Missing or changed qualified Nemotron model' >&2;exit 2; }
                nemotron_assets=(libsound-nemotron-runtime.so NEMOTRON-TRANSCRIBE-LICENSE NEMOTRON-GGML-LICENSE NEMOTRON-THIRD-PARTY-LICENSES)
                for asset in "${nemotron_assets[@]}"; do
                    file="$candidate/bin/64bit/stt-runtime/$asset"
                    [[ -f "$file" && ! -L "$file" ]] && grep -Fxq "nemotron_asset:$asset=$(sha256sum "$file" | cut -d' ' -f1)" "$receipt" ||
                      { echo "Changed Nemotron runtime asset: $asset" >&2;exit 2; }
                done
            fi
            for asset in libwhisper.so.1 libggml.so.0 libggml-base.so.0 libggml-cpu.so.0 LICENSE; do
                file="$candidate/bin/64bit/stt-runtime/$asset"
                [[ -f "$file" ]] && grep -Fxq "stt_asset:$asset=$(sha256sum "$file" | cut -d' ' -f1)" "$receipt" ||
                  { echo "Changed STT runtime asset: $asset" >&2;exit 2; }
            done
            stt_model="$(sed -n 's/^stt_model=//p' "$receipt")"
            [[ -f "$stt_model" ]] && grep -Fxq "stt_model_sha256=$(sha256sum "$stt_model" | cut -d' ' -f1)" "$receipt" ||
              { echo 'Missing or changed qualified STT model' >&2;exit 2; }
        fi
        grep -Fxq 'tts=SOUND_COMBINED_TTS_PASS=1' "$receipt" ||
          { echo 'Missing unified TTS qualification' >&2;exit 2; }
        worker="$(cd "$(dirname "$0")/.." && pwd)/tools/tts_worker.py"
        grep -Fxq "tts_worker_sha256=$(sha256sum "$worker" | cut -d' ' -f1)" "$receipt" ||
          { echo 'Changed TTS worker' >&2;exit 2; }
        ;;
    *) echo 'Unsupported combined effect set' >&2;exit 2 ;;
esac
for line in "effects=$effects" \
 "obs_check=SOUND_REGISTRY_MATCH=1 EXPECTED=[$effects] ACTUAL_COUNT=$count" \
 'real_capture=SOUND_STEM_LIVE_REAL_GPU_OBS_PASS=1' \
 'yamnet=SOUND_COMBINED_YAMNET_PASS=1' 'stem=SOUND_STEM_ISOLATED_PASS=1'; do
  grep -Fxq "$line" "$receipt" ||
    { echo "Incomplete combined qualification: $line" >&2;exit 2; }
done
# The model and runtime are user-owned external dependencies. Verify the
# exact qualified files before offering installation, without copying them.
for key in yamnet_model mediapipe_library; do
  path="$(sed -n "s/^$key=//p" "$receipt")"
  expected="$(sed -n "s/^${key}_sha256=//p" "$receipt")"
  [[ -f "$path" && "$expected" =~ ^[a-f0-9]{64}$ ]] &&
    [[ "$(sha256sum "$path" | cut -d' ' -f1)" = "$expected" ]] ||
      { echo "Missing or changed qualified $key" >&2;exit 2; }
done
strings "$source" | grep -Fx "$marker" >/dev/null ||
  { echo 'Binary marker does not match receipt' >&2;exit 2; }
dest="$HOME/.config/obs-studio/plugins/live-latent-lab-sound"
echo "Verified combined Sound candidate: $source"
echo "Destination (user profile): $dest"
echo "Backup on apply: $HOME/.config/obs-studio/plugin-backups/sound-before-combined-*"
# This check is mandatory even for --apply dry run during validation.
if pgrep -u "$(id -u)" -x obs >/dev/null; then
  echo 'OBS is running. Quit it fully before installing; no files were changed.' >&2;exit 3
fi
if [[ "$apply" != '--apply' ]]; then
  echo 'Dry run only: nothing installed. Run again with --apply when OBS is closed.'
  exit 0
fi
# Do not deploy controls against an old already-running Python worker that
# silently ignores them. Explicitly restart the user services before apply.
if [[ "$native_options_required" == true ]]; then
  for service in lll-sound-tts lll-sound-chatterbox lll-sound-edge; do
    socket="${XDG_RUNTIME_DIR:-/nonexistent}/$service/speech.sock"
    if ! curl --noproxy '*' -fsS --max-time 3 --unix-socket "$socket" \
      -D - -o /dev/null http://localhost/healthz | tr -d '\r' | grep -Eix 'X-Sound-TTS-Options: *1' >/dev/null; then
      echo "Native speech options worker unavailable or outdated: $service. Restart the speech services before installing." >&2
      exit 2
    fi
  done
fi
if [[ -L "$dest" || -L "$dest/bin" || -L "$dest/bin/64bit" ||
      -L "$dest/bin/64bit/live-latent-lab-sound.so" ||
      -L "$dest/data" || -L "$dest/data/locale" ||
      -L "$dest/data/locale/en-US.ini" ]]; then
  echo 'Refusing to modify symlinked plugin installation' >&2;exit 2
fi
if [[ "$stt_required" == true ]]; then
  d="$dest/bin/64bit/stt-runtime"
  [[ ! -L "$d" ]] || { echo 'Refusing symlinked STT runtime' >&2;exit 2; }
  for asset in libwhisper.so.1 libggml.so.0 libggml-base.so.0 libggml-cpu.so.0 LICENSE "${nemotron_assets[@]}"; do
    [[ ! -L "$d/$asset" ]] || { echo 'Refusing symlinked STT runtime asset' >&2;exit 2; }
  done
fi
# Preserve full prior user plugin folder including models and libs, even though
# the release only touches its binary + locale. Never overwrite backup folders.
backup_root="$HOME/.config/obs-studio/plugin-backups"
mkdir -p "$backup_root"
backup="$(mktemp -d "$backup_root/sound-before-combined-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
if [[ -d "$dest" ]]; then cp -a "$dest/." "$backup/"; fi
if [[ "$stt_required" == true ]]; then
  for asset in libwhisper.so.1 libggml.so.0 libggml-base.so.0 libggml-cpu.so.0 LICENSE; do
    install -Dm644 "$candidate/bin/64bit/stt-runtime/$asset" "$dest/bin/64bit/stt-runtime/$asset"
    grep -Fxq "stt_asset:$asset=$(sha256sum "$dest/bin/64bit/stt-runtime/$asset" | cut -d' ' -f1)" "$receipt"
  done
fi
for asset in "${nemotron_assets[@]}"; do
  install -Dm644 "$candidate/bin/64bit/stt-runtime/$asset" "$dest/bin/64bit/stt-runtime/$asset"
  grep -Fxq "nemotron_asset:$asset=$(sha256sum "$dest/bin/64bit/stt-runtime/$asset" | cut -d' ' -f1)" "$receipt"
done
install -Dm644 "$source" "$dest/bin/64bit/live-latent-lab-sound.so"
locale="$candidate/data/locale/en-US.ini"
if [[ -r "$locale" ]]; then
  install -Dm644 "$locale" "$dest/data/locale/en-US.ini"
fi
test "$(sha256sum "$dest/bin/64bit/live-latent-lab-sound.so" | cut -d' ' -f1)" = "$sha"
echo "INSTALLED_SOUND_SHA256=$sha"
echo "BACKUP=$backup"
echo "Next: restart OBS, then verify its log contains $marker"
