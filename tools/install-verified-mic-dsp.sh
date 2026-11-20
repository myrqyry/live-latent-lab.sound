#!/usr/bin/env bash
# Intentional install of an already tested mic-only candidate; not an updater.
# It never stops OBS, installs system-wide, or removes other plugin files.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 CANDIDATE_DIR [--apply]" >&2;exit 2
fi
candidate="$(realpath -m "$1")"
apply="${2:-}"
[[ -z "$apply" || "$apply" == '--apply' ]] || { echo 'Only --apply is supported' >&2;exit 2; }
source="$candidate/bin/64bit/live-latent-lab-sound.so"
receipt="$candidate/verified-mic-dsp.txt"
[[ -f "$source" && -f "$receipt" && ! -L "$source" ]] ||
  { echo 'Missing verified candidate binary/receipt' >&2;exit 2; }
sha="$(sha256sum "$source" | cut -d' ' -f1)"
grep -Fxq "sha256=$sha" "$receipt" ||
  { echo 'Candidate SHA256 does not match its verification receipt' >&2;exit 2; }
nonce="$(sed -n 's/^nonce=//p' "$receipt")"
[[ "$nonce" =~ ^[a-f0-9]{16}$ ]] ||
  { echo 'Invalid or missing candidate nonce' >&2;exit 2; }
grep -Fxq "marker=SOUND_MIC_DSP_CANDIDATE_$nonce" "$receipt" ||
  { echo 'Candidate does not declare the verified mic-only marker' >&2;exit 2; }
grep -Fxq 'registry=SOUND_REGISTRY_MATCH=1 EXPECTED=[mic] ACTUAL_COUNT=1' "$receipt" ||
  { echo 'Candidate is not mic-only' >&2;exit 2; }
dest="$HOME/.config/obs-studio/plugins/live-latent-lab-sound"
echo "Verified mic-only candidate: $source"
echo "Destination (user profile): $dest"
echo "Backup on apply: $HOME/.config/obs-studio/plugin-backups/sound-before-mic-*"
# This check is mandatory even for --apply dry run during validation.
if pgrep -u "$(id -u)" -x obs >/dev/null; then
  echo 'OBS is running. Quit it fully before installing; no files were changed.' >&2;exit 3
fi
if [[ "$apply" != '--apply' ]]; then
  echo 'Dry run only: nothing installed. Run again with --apply when OBS is closed.'
  exit 0
fi
if [[ -L "$dest" || -L "$dest/bin" || -L "$dest/bin/64bit" ||
      -L "$dest/bin/64bit/live-latent-lab-sound.so" ||
      -L "$dest/data" || -L "$dest/data/locale" ||
      -L "$dest/data/locale/en-US.ini" ]]; then
  echo 'Refusing to modify symlinked plugin installation' >&2;exit 2
fi
# Preserve full prior user plugin folder including models and libs, even though
# the release only touches its binary + locale. Never overwrite backup folders.
backup_root="$HOME/.config/obs-studio/plugin-backups"
mkdir -p "$backup_root"
backup="$(mktemp -d "$backup_root/sound-before-mic-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")"
if [[ -d "$dest" ]]; then cp -a "$dest/." "$backup/"; fi
install -Dm644 "$source" "$dest/bin/64bit/live-latent-lab-sound.so"
locale="$candidate/data/locale/en-US.ini"
if [[ -r "$locale" ]]; then
  install -Dm644 "$locale" "$dest/data/locale/en-US.ini"
fi
test "$(sha256sum "$dest/bin/64bit/live-latent-lab-sound.so" | cut -d' ' -f1)" = "$sha"
echo "INSTALLED_MIC_DSP_SHA256=$sha"
echo "BACKUP=$backup"
echo "Next: restart OBS, then verify its log contains SOUND_MIC_DSP_CANDIDATE_$nonce"
