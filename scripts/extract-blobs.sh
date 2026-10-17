#!/usr/bin/env bash
# Rebuilds every proprietary file the KitKat build needs from Google's official Pixel 2 XL
# factory image (RP1A.201005.004.A1, Android 11). Nothing proprietary lives in this repository.
#
#   scripts/extract-blobs.sh <taimen-rp1a.201005.004.a1-factory-*.zip> <aosp dir>
#
# Output: <aosp>/vendor/google/taimen/ (a11/, v11/, firmware/, modules/, kernel, ...)
# Work files (about 6 GB) go to $WORK (default: <aosp>/out-blobs) and can be deleted afterwards.
# Needs: unzip, simg2img, debugfs (e2fsprogs), unpack_bootimg, lz4, python3.
set -euo pipefail

FACTORY=${1:?usage: $0 <factory zip> <aosp dir>}
AOSP=$(realpath "${2:?usage: $0 <factory zip> <aosp dir>}")
HERE=$(dirname "$(realpath "$0")")
WORK=${WORK:-$AOSP/out-blobs}
DEST=$AOSP/vendor/google/taimen
EXPECTED_SHA256=2f5c4987a8e1c29bb2843d7f7d36721d52f2eb25a0fe4c74b3590e34a06eb44f

for t in unzip simg2img debugfs unpack_bootimg lz4 python3 sha256sum; do
    command -v $t >/dev/null || { echo "missing tool: $t" >&2; exit 1; }
done

step() { printf '\n==> %s\n' "$*"; }

step "Checking the factory image"
case "$(basename "$FACTORY")" in
    taimen-rp1a.201005.004.a1-factory-*.zip) ;;
    *) echo "expected taimen-rp1a.201005.004.a1-factory-*.zip (the exact build matters)" >&2; exit 1 ;;
esac
if [ "${SKIP_SHA256:-0}" != 1 ]; then
    sum=$(sha256sum "$FACTORY" | cut -d' ' -f1)
    [ "$sum" = $EXPECTED_SHA256 ] || { echo "SHA-256 mismatch: $sum (expected $EXPECTED_SHA256)" >&2; exit 1; }
fi

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f image.zip ]; then
    step "Unpacking the factory zip"
    unzip -p "$FACTORY" '*/image-taimen-*.zip' > image.zip.part && mv image.zip.part image.zip
fi
for img in boot system vendor; do
    [ -f $img.img ] || unzip -o -q image.zip $img.img
done

for img in system vendor; do
    if [ ! -d $img ]; then
        step "Extracting $img.img"
        simg2img $img.img $img.raw
        mkdir -p $img.tmp
        # system.img is system-as-root: the Android /system tree is its /system directory
        if [ $img = system ]; then src=/system; else src=/; fi
        debugfs -R "rdump $src $PWD/$img.tmp" $img.raw >/dev/null 2>&1
        if [ $img = system ]; then mv system.tmp/system system; rmdir system.tmp; else mv vendor.tmp vendor; fi
        rm -f $img.raw
    fi
done

if [ ! -d apex ]; then
    step "Extracting the APEX modules (bionic, libc++, ICU, VNDK...)"
    mkdir -p apex.tmp
    for a in system/apex/*.apex; do
        n=$(basename "$a" .apex)
        unzip -o -q "$a" apex_payload.img -d apex.tmp
        mkdir -p apex.tmp/$n
        debugfs -R "rdump / $PWD/apex.tmp/$n" apex.tmp/apex_payload.img >/dev/null 2>&1
        rm apex.tmp/apex_payload.img
    done
    mv apex.tmp apex
fi

if [ ! -f kernel.lz4 ]; then
    step "Extracting the kernel"
    rm -rf boot && unpack_bootimg --boot_img boot.img --out boot >/dev/null
    mv boot/kernel kernel.lz4
fi

step "Copying the blobs to $DEST"
python3 -I "$HERE/extract.py" "$HERE/proprietary-files.txt" "$WORK" "$DEST"

step "Patching the kernel (skip_initramfs -> want_initramfs)"
python3 -I "$HERE/patch-kernel.py" kernel.lz4 "$DEST/kernel"

echo
echo "Done. Proprietary files are in $DEST"
echo "The work directory $WORK can be deleted."
