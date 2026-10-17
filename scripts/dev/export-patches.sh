#!/usr/bin/env bash
# Developer tool: regenerates patches/ from the commits on top of android-4.4.4_r2 in each
# patched AOSP project (after editing them there).
#   scripts/dev/export-patches.sh <aosp dir>
set -euo pipefail
AOSP=$(realpath "${1:?usage: $0 <aosp dir>}")
OUT=$(dirname "$(dirname "$(dirname "$(realpath "$0")")")")/patches
for d in $(cd "$OUT" && find . -name '*.patch' -printf '%h\n' | sort -u); do
    p=${d#./}
    rm -f "$OUT/$p"/*.patch
    git -C "$AOSP/$p" format-patch -q --zero-commit --no-signature -o "$OUT/$p" m/android-4.4.4_r2..HEAD
    echo "$p: $(ls "$OUT/$p" | wc -l)"
done
