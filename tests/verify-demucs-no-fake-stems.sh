#!/usr/bin/env bash
# Prevent model failure from being misreported as four separated audio streams.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d /tmp/lll-demucs-guard-XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I"$root/src" \
  "$root/tests/verify-demucs-no-fake-stems.cpp" -o "$tmp/guard"
"$tmp/guard"
# Also reject the historic fabricated fallbacks in the actual producer files.
if grep -E 'return \{input, input, input, input\}|result\[i\] = input;|stems\[i\] = current_chunk;' \
    "$root/src/engines/demucs_engine.hpp" \
    "$root/src/engines/demucs_cpp_engine.hpp" \
    "$root/src/demucs-source.cpp" "$root/src/demucs-filter.cpp"; then
  echo 'DEMUCS_FAKE_STEMS_FALLBACK_REMAINING=1' >&2
  exit 1
fi
if grep -Fq 'for (float s : current_chunk)' "$root/src/demucs-filter.cpp"; then
  echo 'DEMUCS_FILTER_FALSE_MIX_FALLBACK_REMAINING=1' >&2
  exit 1
fi
