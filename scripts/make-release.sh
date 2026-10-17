#!/usr/bin/env bash
# Packages a build for users: a fastboot package (images + flash-all scripts) and checksums.
#   scripts/make-release.sh <aosp dir> <output dir> [name]    (name default: date)
set -euo pipefail
AOSP=$(realpath "${1:?usage: $0 <aosp dir> <output dir> [name]}")
OUT=$(realpath -m "${2:?usage: $0 <aosp dir> <output dir> [name]}")
NAME=Taimen-KitKat-${3:-$(date -u +%Y%m%d)}
REPO=$(dirname "$(dirname "$(realpath "$0")")")
P=$AOSP/out/target/product/taimen-kk
for t in zip sha256sum; do command -v $t >/dev/null || { echo "missing tool: $t" >&2; exit 1; }; done
grep -q '^ro.build.type=user$' $P/system/build.prop || { echo "not a user build" >&2; exit 1; }
rm -rf "$OUT/$NAME-fastboot" && mkdir -p "$OUT/$NAME-fastboot"

echo "==> fastboot package"
F=$OUT/$NAME-fastboot
cp $P/boot.img $P/system.img "$F/"
cp "$REPO/release/flash-all.sh" "$REPO/release/flash-all.bat" "$F/"
(cd "$F" && sha256sum boot.img system.img > images.sha256)
(cd "$OUT" && rm -f "$NAME-fastboot.zip" && zip -q -r "$NAME-fastboot.zip" "$NAME-fastboot")

(cd "$OUT" && sha256sum "$NAME-fastboot.zip" > "$NAME.sha256")
rm -rf "$F"
ls -l "$OUT/$NAME"*
