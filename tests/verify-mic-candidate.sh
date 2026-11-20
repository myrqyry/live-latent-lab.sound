#!/usr/bin/env bash
# Verified opt-in mic DSP candidate must identify its exact runtime binary.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d /tmp/lll-sound-mic-candidate-test-XXXXXX)"
trap 'rm -rf "$work"' EXIT
"$root/tools/stage-verified-mic-dsp.sh" "$work/release"
module="$work/release/bin/64bit/live-latent-lab-sound.so"
receipt="$work/release/verified-mic-dsp.txt"
test -s "$module" && test -s "$receipt"
sha="$(sha256sum "$module" | cut -d' ' -f1)"
grep -F "sha256=$sha" "$receipt"
nonce="$(sed -n 's/^nonce=//p' "$receipt")"
test "${#nonce}" -ge 12
grep -F "SOUND_MIC_DSP_CANDIDATE_$nonce" "$receipt"
# Installation command must refuse while the user's OBS session is active.
if pgrep -u "$(id -u)" -x obs >/dev/null; then
  if "$root/tools/install-verified-mic-dsp.sh" "$work/release" --apply >"$work/install.log" 2>&1; then
    echo 'ERROR installer modified active OBS' >&2;exit 1
  fi
  grep -q 'OBS is running' "$work/install.log"
fi
# Exercise the opt-in install in a completely disposable HOME. This does NOT
# terminate or modify the user's active OBS process or plugin folders.
mkdir -p "$work/fake-bin" "$work/fake-home/.config/obs-studio/plugins/live-latent-lab-sound/bin/64bit"
printf 'original plugin bytes' >"$work/fake-home/.config/obs-studio/plugins/live-latent-lab-sound/bin/64bit/live-latent-lab-sound.so"
printf '#!/bin/sh\nexit 1\n' >"$work/fake-bin/pgrep"
chmod +x "$work/fake-bin/pgrep"
env HOME="$work/fake-home" PATH="$work/fake-bin:$PATH" \
  "$root/tools/install-verified-mic-dsp.sh" "$work/release" --apply >"$work/fake-activate.log"
installed="$work/fake-home/.config/obs-studio/plugins/live-latent-lab-sound/bin/64bit/live-latent-lab-sound.so"
test "$(sha256sum "$installed" | cut -d' ' -f1)" = "$sha"
backup="$(sed -n 's/^BACKUP=//p' "$work/fake-activate.log")"
test -f "$backup/bin/64bit/live-latent-lab-sound.so"
test "$(cat "$backup/bin/64bit/live-latent-lab-sound.so")" = 'original plugin bytes'
grep -F "INSTALLED_MIC_DSP_SHA256=$sha" "$work/fake-activate.log"
# Never allow installation to write through a pre-existing locale symlink.
locale_parent="$work/fake-home/.config/obs-studio/plugins/live-latent-lab-sound/data"
mv "$locale_parent/locale" "$locale_parent/locale-safe"
mkdir -p "$work/outside"
ln -s "$work/outside" "$locale_parent/locale"
if env HOME="$work/fake-home" PATH="$work/fake-bin:$PATH" \
  "$root/tools/install-verified-mic-dsp.sh" "$work/release" --apply >"$work/symlink.log" 2>&1; then
  echo "Unsafe locale symlink installation accepted" >&2;exit 1
fi
test ! -f "$work/outside/en-US.ini"
echo "SOUND_MIC_CANDIDATE_SAFE_PASS=1"
