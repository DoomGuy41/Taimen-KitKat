#!/usr/bin/env python3
# The stock kernel ignores the boot.img ramdisk when the bootloader passes "skip_initramfs" (A/B
# system-as-root). KitKat needs its ramdisk, so the parameter name is renamed in the kernel image:
# the bootloader keeps adding skip_initramfs, which the kernel no longer recognizes.
# usage: patch-kernel.py <stock kernel (Image.lz4 + appended DTB)> <output>
import subprocess, sys

stock, out = sys.argv[1:3]
data = open(stock, 'rb').read()
# Image.lz4-dtb: an LZ4 frame followed by the device tree blobs (magic d00dfeed)
dtb = data.find(b'\xd0\x0d\xfe\xed')
lz4, dtbs = data[:dtb], data[dtb:]
image = subprocess.run(['lz4', '-d', '-c'], input=lz4, capture_output=True, check=True).stdout
old, new = b'skip_initramfs\0', b'want_initramfs\0'
if image.count(old) != 1:
    sys.exit('unexpected kernel: %d occurrences of skip_initramfs' % image.count(old))
image = image.replace(old, new)
# The bootloader only reads the LZ4 frame format (not the legacy one), like the stock kernel:
# 4 MiB independent blocks with a content checksum, which are lz4's defaults. Level 9 gives the
# same file as the kernel tested during the port.
packed = subprocess.run(['lz4', '-9', '-c'], input=image, capture_output=True, check=True).stdout
if packed[:4] != b'\x04\x22\x4d\x18':
    sys.exit('lz4 did not produce an LZ4 frame')
open(out, 'wb').write(packed + dtbs)
print('kernel patched: %s (%d bytes)' % (out, len(packed) + len(dtbs)))
