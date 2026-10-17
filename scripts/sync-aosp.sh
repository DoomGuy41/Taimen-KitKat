#!/usr/bin/env bash
# Downloads AOSP android-4.4.4_r2 (about 40 GB) and applies the patches of this repository.
#   scripts/sync-aosp.sh <aosp dir>          (run again to re-apply the patches)
# Needs the tools of setup-host.sh (repo, python): KK_TOOLS (default ~/kk-tools).
set -euo pipefail
KK_TOOLS=${KK_TOOLS:-$HOME/kk-tools}
# shellcheck disable=SC1091
[ -f "$KK_TOOLS/env.sh" ] && { set +u; source "$KK_TOOLS/env.sh"; set -u; }
AOSP=$(realpath -m "${1:?usage: $0 <aosp dir>}")
REPO=$(dirname "$(dirname "$(realpath "$0")")")
JOBS=${JOBS:-8}
mkdir -p "$AOSP"
cd "$AOSP"
if [ ! -d .repo ]; then
    repo init -q -u https://android.googlesource.com/platform/manifest -b android-4.4.4_r2
fi
repo sync -c -q -j"$JOBS" --no-tags

# each patched project gets a "taimen" branch: android-4.4.4_r2 + the patches
for d in $(cd "$REPO/patches" && find . -name '*.patch' -printf '%h\n' | sort -u); do
    p=${d#./}
    git -C "$p" checkout -q -B taimen m/android-4.4.4_r2
    git -C "$p" -c user.name=taimen -c user.email=taimen@localhost am -q --committer-date-is-author-date "$REPO/patches/$p"/*.patch
    echo "patched $p ($(ls "$REPO/patches/$p" | wc -l))"
done
mkdir -p device/google
rsync -a --delete --exclude=/.git "$REPO/device/google/taimen-kk/" device/google/taimen-kk/
echo "AOSP ready in $AOSP"
