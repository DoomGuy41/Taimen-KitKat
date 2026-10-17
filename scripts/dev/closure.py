#!/usr/bin/env python3
# Developer tool: DT_NEEDED closure of Android 11 libraries in an unpacked factory image (the work
# directory of extract-blobs.sh), and the symbols they need that neither the closure nor KitKat's
# own libraries provide: what libhyb's hooks.c has to supply. This is how proprietary-files.txt
# and the hook list were put together.
#   closure.py [--64] <work dir> <KitKat out/target/product/taimen-kk/system/lib> <root lib>...
import glob, os, subprocess, sys

args = sys.argv[1:]
bits64 = args[:1] == ['--64']
if bits64:
    args = args[1:]
work, kk, roots = args[0], args[1], args[2:]
lib = 'lib64' if bits64 else 'lib'
SEARCH = [work + '/vendor/%s/egl' % lib, work + '/vendor/%s/hw' % lib, work + '/vendor/' + lib,
          work + '/system/' + lib] + sorted(glob.glob(work + '/apex/*/' + lib)) + \
         sorted(glob.glob(work + '/apex/*/%s/bionic' % lib))
# provided by KitKat itself (libhyb maps them to the host libraries)
HOST = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libcutils.so', 'libz.so', 'ld-android.so',
        'libdl_android.so'}


def find(name):
    for d in SEARCH:
        if os.path.isfile(os.path.join(d, name)):
            return os.path.join(d, name)


def needed(path):
    out = subprocess.run(['readelf', '-dW', path], capture_output=True, text=True).stdout
    return [l.split('[')[1].split(']')[0] for l in out.splitlines() if '(NEEDED)' in l]


def symbols(path, undefined):
    out = subprocess.run(['readelf', '-W', '--dyn-syms', path], capture_output=True, text=True).stdout
    found = set()
    for l in out.splitlines():
        f = l.split()
        if len(f) < 8:
            continue
        if undefined and f[6] == 'UND':
            found.add(f[7].split('@')[0])
        if not undefined and f[6] != 'UND' and f[4] in ('GLOBAL', 'WEAK'):
            found.add(f[7].split('@')[0])
    return found


seen, queue, missing = {}, list(roots), set()
while queue:
    n = queue.pop()
    if n in seen or n in HOST:
        continue
    p = find(n)
    if not p:
        missing.add(n)
        continue
    seen[n] = p
    queue += needed(p)
provided = set().union(*(symbols(p, False) for p in seen.values()))
host = set().union(*(symbols(os.path.join(kk, h), False) for h in HOST if os.path.isfile(os.path.join(kk, h))))
wanted = set().union(*(symbols(p, True) for p in seen.values()))
gap = sorted(wanted - provided - host)
print('libraries (%d):' % len(seen))
for n in sorted(seen):
    print('  ' + os.path.relpath(seen[n], work))
if missing:
    print('not found:', ' '.join(sorted(missing)))
print('symbols to provide (%d):' % len(gap))
print(' '.join(gap))
