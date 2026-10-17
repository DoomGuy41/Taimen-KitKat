# Building Taimen KitKat

## Host

- An x86_64 Linux machine. The scripts were tested on Ubuntu 26.04 and need `sudo` for apt
  packages only.
- About **150 GB** of free disk space (AOSP checkout 40 GB, build output 60 GB, factory image
  work files 6 GB) and **16 GB of RAM** (or swap and fewer jobs).
- Network access to android.googlesource.com, dl.google.com, sourceforge.net (GApps) and
  ports.ubuntu.com (busybox).

KitKat needs a 2014 toolchain: GNU make 3.82, JDK 6, Python 2.7 and ncurses 5. `setup-host.sh`
builds or downloads them into `~/kk-tools` (checked by signature or checksum) and leaves the
system untouched. It also wraps Zulu 6 so that `java -version` does not say "OpenJDK", which
KitKat's makefiles reject.

```sh
git clone https://github.com/DoomGuy41/Taimen-KitKat
cd Taimen-KitKat
scripts/setup-host.sh
```

The other tools used by the scripts come from apt: `simg2img` (android-sdk-libsparse-utils),
`debugfs` (e2fsprogs), `unpack_bootimg` (mkbootimg), `lz4`, `rsync`, `zip`, `openssl`.

## Steps

```sh
AOSP=~/aosp-kk

# 1. AOSP android-4.4.4_r2, the patches of patches/ (branch "taimen" in each patched project)
#    and a copy of device/google/taimen-kk
scripts/sync-aosp.sh $AOSP

# 2. Proprietary files from Google's factory image -> $AOSP/vendor/google/taimen
scripts/extract-blobs.sh taimen-rp1a.201005.004.a1-factory-2f5c4987.zip $AOSP

# 3. Optional: Google apps (Open GApps 4.4 nano) -> $AOSP/vendor/google/gapps-kk
scripts/setup-gapps.sh $AOSP
#    Optional: the default wallpaper of the release (downloaded, not redistributed)
scripts/fetch-wallpaper.sh $AOSP

# 4. Your own signing keys -> ~/.android-certs/taimen-kk (keep them private)
scripts/make-keys.sh

# 5. Build boot.img and system.img (~1 h the first time)
scripts/build.sh $AOSP user

# 6. Package: fastboot zip + checksums
scripts/make-release.sh $AOSP ~/taimen-release
```

`build.sh` copies `device/google/taimen-kk` of this repository into the AOSP tree on every
run: edit the device tree here, not in `$AOSP`. It also rebuilds the programs compiled with
the NDK (`build-native.sh`), with the NDK r21e downloaded on first use. Environment:

| Variable | Default | |
|---|---|---|
| `KK_TOOLS` | `~/kk-tools` | toolchain installed by `setup-host.sh` |
| `TAIMEN_KEYS` | `~/.android-certs/taimen-kk` | release keys; without them, user builds use the AOSP test keys |
| `JOBS` | number of CPUs | `make -j` |
| `CLEAN=1` | | `make installclean` first (needed after removing files from the tree) |
| `NDK` | downloaded | path to an existing android-ndk-r21e |
| `BUILD_NUMBER` | today's date | |

## Variants

- **user**: the release build. It is signed with your keys and tagged `release-keys`, has
  adb off by default, and includes GApps if `vendor/google/gapps-kk` exists.
- **eng**: the bring-up build. It has adb open without authorization, verbose kernel log and a
  persistent log in `/data/log`. It adds the `TaimenDiag` app (every hardware value and test,
  in plain text) and test programs: `hybtest`, `camtest`, `glbench`, `enctest`, `tinymix`, and
  others.

`scripts/build.sh $AOSP eng` builds it. Switching variants rebuilds what differs, and
`CLEAN=1` is recommended when switching.

Flash a build directly from the output folder:

```sh
cd $AOSP/out/target/product/taimen-kk
fastboot --set-active=a
fastboot flash boot_a boot.img
fastboot flash system_a system.img
fastboot format:ext4 userdata     # first install, or when switching variants or keys
fastboot reboot
```

## Developer tools

| Tool | |
|---|---|
| `scripts/dev/closure.py` | DT_NEEDED closure of Android 11 libraries in the unpacked factory image, and the symbols that libhyb's `hooks.c` must provide. Use it to add a library to `proprietary-files.txt`. |
| `scripts/dev/export-patches.sh` | regenerates `patches/` after committing changes in the AOSP projects |
| `HYB_TRACE=1`, `HYB_TRACE_SYM=<name>` | libhyb logs every dlopen, or who resolves a symbol |
| `debug.egl_shim.trace=1` | logs the query/perform calls of the Adreno driver |
| `debug.camera.taimen.probe=1` | logs dropped metadata and checks returned camera buffers |
| `gnssbridge --test 60`, `nfcbridge --test`, `sensorbridge test <handle>` | drive the bridges directly, inside the chroot: `/system/v11/hwchroot /system/v11/root /vendor/bin/...` |

The kernel log is the logcat: liblog writes to `/dev/kmsg`, so `adb shell dmesg` shows both.
