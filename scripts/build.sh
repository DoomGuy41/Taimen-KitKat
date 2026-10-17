#!/usr/bin/env bash
# Builds boot.img and system.img.
#   scripts/build.sh <aosp dir> [user|eng]      (default: user)
# The device tree of this repository is copied to <aosp>/device/google/taimen-kk first: edit it
# here, not in the AOSP tree. Environment: KK_TOOLS (default ~/kk-tools, see setup-host.sh),
# TAIMEN_KEYS (default ~/.android-certs/taimen-kk), JOBS (default: number of CPUs), CLEAN=1.
set -euo pipefail

AOSP=$(realpath "${1:?usage: $0 <aosp dir> [user|eng]}")
VARIANT=${2:-user}
REPO=$(dirname "$(dirname "$(realpath "$0")")")
KK_TOOLS=${KK_TOOLS:-$HOME/kk-tools}
JOBS=${JOBS:-$(nproc)}

[ -f "$AOSP/vendor/google/taimen/kernel" ] || { echo "run scripts/extract-blobs.sh first" >&2; exit 1; }
[ -f "$AOSP/vendor/google/taimen/v11/hwchroot" ] || { echo "run scripts/build-native.sh first" >&2; exit 1; }

echo "==> Copying the device tree"
mkdir -p "$AOSP/device/google"
rsync -a --delete --exclude=/.git "$REPO/device/google/taimen-kk/" "$AOSP/device/google/taimen-kk/"
# build-native.sh compiles from the AOSP copy: refresh the binaries if sources changed
"$REPO/scripts/build-native.sh" "$AOSP" >/dev/null

# Reproducible build properties: no local user or host name in build.prop
fake=$(mktemp -d)
printf '#!/bin/sh\necho taimen-build\n' > "$fake/hostname" && chmod +x "$fake/hostname"
trap 'rm -rf "$fake"' EXIT

cd "$AOSP"
# build.prop is only regenerated when it is missing
rm -f out/target/product/taimen-kk/system/build.prop
set +u
# shellcheck disable=SC1091
source "$KK_TOOLS/env.sh"
source build/envsetup.sh >/dev/null
lunch aosp_taimen_kk-$VARIANT >/dev/null
set -u
export USER=android-build BUILD_NUMBER=${BUILD_NUMBER:-$(date -u +%Y%m%d)} PATH=$fake:$PATH
mkdir -p out
# CLEAN=1: remove what previous builds installed (files dropped from the tree stay otherwise)
[ "${CLEAN:-0}" = 1 ] && make installclean > out/installclean.log 2>&1
echo "==> Building aosp_taimen_kk-$VARIANT ($JOBS jobs, log: $AOSP/out/build-$VARIANT.log)"
if ! make -j"$JOBS" bootimage systemimage > out/build-$VARIANT.log 2>&1; then
    grep -E 'error|Error' out/build-$VARIANT.log | tail -20
    exit 1
fi
P=out/target/product/taimen-kk
grep -E '^ro\.build\.(fingerprint|tags)=' $P/system/build.prop
ls -l $P/boot.img $P/system.img
