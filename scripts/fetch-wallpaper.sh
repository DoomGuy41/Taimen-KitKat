#!/usr/bin/env bash
# Downloads the default wallpaper of the release build (not redistributed here). Without it the
# build falls back to the stock AOSP KitKat wallpaper.
#   scripts/fetch-wallpaper.sh <aosp dir>
set -euo pipefail
AOSP=$(realpath "${1:?usage: $0 <aosp dir>}")
URL=https://i.redd.it/3mud7q6w1j811.jpg
SHA256=9ddcbb98e2a8e061e296aa058e5e674a50105c5483936fd75188a20dfa7bc885
D=$AOSP/vendor/google/taimen/overlay/frameworks/base/core/res/res/drawable-nodpi
mkdir -p "$D"
curl -fL -o "$D/default_wallpaper.jpg.part" "$URL"
if ! echo "$SHA256  $D/default_wallpaper.jpg.part" | sha256sum -c --quiet; then
    echo "warning: the wallpaper changed upstream, using it anyway" >&2
fi
mv "$D/default_wallpaper.jpg.part" "$D/default_wallpaper.jpg"
echo "wallpaper: $D/default_wallpaper.jpg"
