#!/usr/bin/env python3
# Copies the files listed in proprietary-files.txt from an unpacked factory image.
# usage: extract.py <proprietary-files.txt> <unpacked factory image> <destination>
import os, shutil, sys

listing, src, dest = sys.argv[1:4]
copied = 0
missing = []
for line in open(listing):
    f = line.split()
    if not f or f[0].startswith('#'):
        continue
    if f[0] == 'dir':
        os.makedirs(os.path.join(dest, f[1]), exist_ok=True)
        continue
    if f[0] == 'link':
        d = os.path.join(dest, f[1])
        os.makedirs(os.path.dirname(d), exist_ok=True)
        if os.path.lexists(d):
            os.remove(d)
        os.symlink(f[2], d)
        continue
    s, d = os.path.join(src, f[0]), os.path.join(dest, f[1] if len(f) > 1 else f[0])
    if not os.path.isfile(s):
        missing.append(f[0])
        continue
    os.makedirs(os.path.dirname(d), exist_ok=True)
    if os.path.lexists(d):
        os.remove(d)
    shutil.copyfile(s, d)
    shutil.copymode(s, d)
    copied += 1
print('%d files copied' % copied)
if missing:
    sys.exit('missing from the factory image:\n  ' + '\n  '.join(missing))
