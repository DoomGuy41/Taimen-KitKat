#!/usr/bin/env bash
# Optional: Google services for the release build (Open GApps 4.4 nano, the largest package that
# exists for KitKat). Produces <aosp>/vendor/google/gapps-kk, picked up by user builds only.
#   scripts/setup-gapps.sh <aosp dir>
set -euo pipefail
AOSP=$(realpath "${1:?usage: $0 <aosp dir>}")
HERE=$(dirname "$(realpath "$0")")
CACHE=${CACHE:-$AOSP/out-blobs}/gapps
ZIP_URL=https://sourceforge.net/projects/opengapps/files/arm/20220215/open_gapps-arm-4.4-nano-20220215.zip/download
ZIP_SHA256=4f778f1d97efc9193311a1cb170c15917a6689d61a72cf829406b762f7383fb4
LZIP_URL=https://download.savannah.gnu.org/releases/lzip/lzip-1.24.1.tar.gz
LZIP_SHA256=30c9cb6a0605f479c496c376eb629a48b0a1696d167e3c1e090c5defa481b162
mkdir -p "$CACHE"
cd "$CACHE"

ZIP=open_gapps-arm-4.4-nano-20220215.zip
if [ ! -f $ZIP ]; then
    echo "==> Downloading Open GApps"
    curl -fL -o $ZIP.part "$ZIP_URL" && mv $ZIP.part $ZIP
fi
echo "$ZIP_SHA256  $ZIP" | sha256sum -c --quiet

# the packages inside are .tar.lz
LZIP=$(command -v lzip || true)
if [ -z "$LZIP" ]; then
    if [ ! -x lzip-1.24.1/lzip ]; then
        echo "==> Building lzip"
        curl -fL -o lzip.tar.gz "$LZIP_URL"
        echo "$LZIP_SHA256  lzip.tar.gz" | sha256sum -c --quiet
        tar -xzf lzip.tar.gz && (cd lzip-1.24.1 && ./configure >/dev/null && make -s)
    fi
    LZIP=$PWD/lzip-1.24.1/lzip
fi

echo "==> Unpacking"
rm -rf pkg x && mkdir -p x
unzip -q $ZIP -d pkg
for f in pkg/Core/*.tar.lz pkg/GApps/*.tar.lz; do
    "$LZIP" -dc "$f" | tar -x -C x
done
python3 -I "$HERE/make-gapps-kk.py" x "$AOSP/vendor/google/gapps-kk"
rm -rf pkg x
