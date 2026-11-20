#!/usr/bin/env bash
# Hermetic C++ SOLA checks; optional real 40k PCM16 WAV to measure/stage A/B.
# Neither OBS nor a voice model is loaded by this script.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -ne 0 && $# -ne 2 ]]; then
    if [[ $# -ne 5 || "$1" != "--independent" ]]; then
        echo "Usage: $0 [40k-mono-pcm16.wav audition-output-directory]" >&2
        echo "   or: $0 --independent clip-a.wav clip-b.wav full-reference.wav output-directory" >&2
        exit 2
    fi
fi
temp="$(mktemp -d /tmp/lll-sola-align-XXXXXX)"
trap 'rm -rf "$temp"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    -I"$root/src" "$root/tests/verify-sola-alignment.cpp" \
    -o "$temp/verify-sola"
"$temp/verify-sola" "$@"
