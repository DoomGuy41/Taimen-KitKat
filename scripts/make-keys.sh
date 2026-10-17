#!/usr/bin/env bash
# Creates your own private signing keys for user builds (releasekey, platform, shared, media).
# Keep them private and back them up: an update signed with other keys cannot replace the
# installed system apps without wiping /data.
#   scripts/make-keys.sh [dir]     (default: ~/.android-certs/taimen-kk, read by the build)
set -euo pipefail
DIR=${1:-$HOME/.android-certs/taimen-kk}
SUBJECT=${SUBJECT:-/O=Taimen KitKat/CN=Taimen KitKat}
mkdir -p "$DIR" && chmod 700 "$DIR"
umask 077
for k in releasekey platform shared media; do
    if [ -e "$DIR/$k.pk8" ]; then echo "$DIR/$k.pk8 exists, kept"; continue; fi
    tmp=$(mktemp)
    openssl genrsa -f4 -out "$tmp" 2048 2>/dev/null
    # KitKat signapk and PackageManager expect SHA1withRSA certificates
    openssl req -new -x509 -sha1 -key "$tmp" -days 10000 -subj "$SUBJECT" -out "$DIR/$k.x509.pem"
    openssl pkcs8 -in "$tmp" -topk8 -outform DER -nocrypt -out "$DIR/$k.pk8"
    rm -f "$tmp"
    chmod 644 "$DIR/$k.x509.pem"
    echo "created $DIR/$k"
done
# external/sepolicy (mac_permissions.xml) looks for testkey.x509.pem next to the default key
ln -sf releasekey.x509.pem "$DIR/testkey.x509.pem"
