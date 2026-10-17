#!/usr/bin/env bash
# Builds the programs that run outside the KitKat build system, with the Android NDK r21e:
#  - in the Android 11 chroot (64-bit, Android 11 bionic): hwchroot, sensorbridge, gnssbridge,
#    nfcbridge, bootmark, libperipheral_client.so (pm-service shim)
#  - on the KitKat side: fbpresentd (64-bit, static), logdw (32-bit, static)
# and downloads busybox (Ubuntu's busybox-static for armhf, checked by SHA-256).
#
#   scripts/build-native.sh <aosp dir>        (NDK=<path to android-ndk-r21e> to reuse one)
set -euo pipefail

AOSP=$(realpath "${1:?usage: $0 <aosp dir>}")
SRC=$AOSP/device/google/taimen-kk
DEST=$AOSP/vendor/google/taimen
CACHE=${CACHE:-$AOSP/out-blobs}
NDK_URL=https://dl.google.com/android/repository/android-ndk-r21e-linux-x86_64.zip
NDK_SHA1=c3ebc83c96a4d7f539bd72c241b2be9dcd29bda9
BUSYBOX_URL=http://ports.ubuntu.com/pool/main/b/busybox/busybox-static_1.37.0-7ubuntu1_armhf.deb
BUSYBOX_SHA256=911941f13fd42957875c1bf3d249346b1f08e981df84a2a66c51364149cdf0e0

[ -d "$SRC" ] || { echo "$SRC not found: copy the device tree first (see BUILDING.md)" >&2; exit 1; }
mkdir -p "$CACHE"

if [ -z "${NDK:-}" ]; then
    NDK=$CACHE/android-ndk-r21e
    if [ ! -d "$NDK" ]; then
        echo "==> Downloading the NDK r21e"
        curl -fL -o "$CACHE/ndk.zip" "$NDK_URL"
        echo "$NDK_SHA1  $CACHE/ndk.zip" | sha1sum -c --quiet
        unzip -q "$CACHE/ndk.zip" -d "$CACHE" && rm "$CACHE/ndk.zip"
    fi
fi
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC64="$TC/aarch64-linux-android29-clang -O2 -Wall"
CC32="$TC/armv7a-linux-androideabi21-clang -O2 -Wall"
V11=$DEST/v11
mkdir -p "$V11/root/vendor/bin" "$V11/root/vendor/lib64" "$DEST/bin"

echo "==> Building the chroot programs"
$CC64 -static -o "$V11/hwchroot" "$SRC/native/hwchroot.c"
$CC64 -I"$SRC/sensors" -o "$V11/root/vendor/bin/sensorbridge" "$SRC/sensors/bridge/sensorbridge.c" -ldl
$CC64 -I"$SRC/gps" -o "$V11/root/vendor/bin/gnssbridge" "$SRC/gps/bridge/gnssbridge.c" -ldl
# -rdynamic: nfcbridge_hidl.cpp overrides getRawServiceInternal of libhidlbase (see that file)
$CC64 -I"$SRC/nfc" -rdynamic -o "$V11/root/vendor/bin/nfcbridge" "$SRC/nfc/bridge/nfcbridge.c" \
    "$SRC/nfc/bridge/nfcbridge_hidl.cpp" -ldl
$CC64 -rdynamic -o "$V11/root/vendor/bin/bootmark" "$SRC/bootmark/bootmark.c" -ldl
$CC64 -shared -fPIC -Wl,-soname,libperipheral_client.so -o "$V11/root/vendor/lib64/libperipheral_client.so" \
    "$SRC/native/pm_shim.c"

echo "==> Building the KitKat-side programs"
$CC64 -static -o "$DEST/bin/fbpresentd" "$SRC/fbpresentd/fbpresentd.c"
$CC32 -static -o "$DEST/bin/logdw" "$SRC/logdw.c"

if [ ! -f "$DEST/bin/busybox" ]; then
    echo "==> Downloading busybox"
    tmp=$(mktemp -d)
    curl -fL -o "$tmp/busybox.deb" "$BUSYBOX_URL"
    (cd "$tmp" && ar x busybox.deb && tar -xf data.tar.* ./usr/bin/busybox)
    echo "$BUSYBOX_SHA256  $tmp/usr/bin/busybox" | sha256sum -c --quiet
    install -m 755 "$tmp/usr/bin/busybox" "$DEST/bin/busybox"
    rm -rf "$tmp"
fi
echo "Done: $DEST"
