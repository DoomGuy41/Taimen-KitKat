#!/bin/sh
# Taimen KitKat: flashes boot.img and system.img to slot A of a Pixel 2 XL in fastboot mode and
# formats userdata. Prerequisites: unlocked bootloader and the factory image RP1A.201005.004.A1
# flashed on slot A (see FLASHING.md). Erases all user data.
cd "$(dirname "$0")"
fail() { echo "ERROR: $*"; exit 1; }
command -v fastboot >/dev/null || fail "fastboot not found (Android platform-tools)"
v=$(fastboot --version | sed -n 's/^fastboot version \([0-9]*\)\.\([0-9]*\)\.\([0-9]*\).*/\1\2/p')
[ -n "$v" ] && [ "$v" -ge 330 ] || fail "fastboot too old, get the latest platform-tools"
get() { fastboot getvar "$1" 2>&1 | sed -n "s/^$1: *//p" | head -1; }
[ "$(get product)" = taimen ] || fail "no Pixel 2 XL (taimen) in fastboot mode"
[ "$(get unlocked)" = yes ] || fail "the bootloader is locked: fastboot flashing unlock first"
[ "$(get version-bootloader)" = TMZ30m ] || fail "bootloader $(get version-bootloader), TMZ30m required: flash RP1A.201005.004.A1 first"
[ "$(get version-baseband)" = g8998-00034-2006052136 ] || fail "radio $(get version-baseband), g8998-00034-2006052136 required: flash RP1A.201005.004.A1 first"
if command -v sha256sum >/dev/null; then sha256sum -c --quiet images.sha256 || fail "checksum mismatch, download again"
elif command -v shasum >/dev/null; then shasum -a 256 -c images.sha256 >/dev/null || fail "checksum mismatch, download again"; fi
echo "This erases all data on the phone. Press Enter to continue, Ctrl+C to cancel."
read _
fastboot --set-active=a || fail "set-active"
fastboot flash boot_a boot.img || fail "boot_a"
fastboot flash system_a system.img || fail "system_a"
fastboot format:ext4 userdata || fail "userdata"
fastboot reboot
echo "Done. The first boot takes about 2 minutes."
