#!/usr/bin/env bash
# =============================================================================
#  setup-host.sh
#  Prepares an Ubuntu x86_64 machine (tested on 26.04) to build AOSP 4.4.4 (KitKat)
#  without Docker. All the old tools go to ~/kk-tools and are only active once
#  env.sh is sourced: the system itself is left untouched.
#  Usage:   bash scripts/setup-host.sh       (safe to run again)
# =============================================================================
set -Eeuo pipefail

KK="${KK_TOOLS:-$HOME/kk-tools}"
SRC="$KK/src"; BUILD="$KK/build"; LOGS="$KK/logs"
JOBS="$(nproc)"

ZULU_URL="https://cdn.azul.com/zulu/bin/zulu6.22.0.3-ca-jdk6.0.119-linux_x64.tar.gz"
ZULU_SHA256="dd4765b45d9f1fc319a6e5be0bbc79371582daa8fd77872d51072f16deca2625"
MAKE_URL="https://ftp.gnu.org/gnu/make/make-3.82.tar.gz"
GNU_KEYRING_URL="https://ftp.gnu.org/gnu/gnu-keyring.gpg"
PY_URL="https://www.python.org/ftp/python/2.7.18/Python-2.7.18.tgz"
PY_MD5="38c84292658ed4456157195f1c9bcbe1"
PLATFORM_TOOLS_URL="https://dl.google.com/android/repository/platform-tools-latest-linux.zip"
REPO_LAUNCHER_URL="https://storage.googleapis.com/git-repo-downloads/repo"
UBUNTU_POOL="https://archive.ubuntu.com/ubuntu/pool"
NCURSES_DEBS=(
  "universe/n/ncurses/libtinfo5_6.3-2ubuntu0.3_amd64.deb|4df4288404108f1a156d014e8764a064e977e34e6d44931ab60451694c03c90d"
  "universe/n/ncurses/libncurses5_6.3-2ubuntu0.3_amd64.deb|58da01991712c6856af0658dc5e9cb533c1a6854ab950055ade185e35bc9c276"
  "main/n/ncurses/lib32tinfo5_6.1-1ubuntu1.18.04.1_amd64.deb|baeb50fe00f1cb9845ca1f9f39313197ef06b7122dc537ac9a256f53909dfd63"
  "main/n/ncurses/lib32ncurses5_6.1-1ubuntu1.18.04.1_amd64.deb|7786e208d10f1f96c8689534b8cafc1c710c0025bdee1dd5f4e541bf22e71dd8"
)
APT_REQUIRED=(
  build-essential gcc-multilib g++-multilib libc6-dev-i386
  git gnupg curl ca-certificates perl python3 unzip zip rsync openssl e2fsprogs lz4
  flex bison gperf zlib1g-dev lib32z1-dev libncurses-dev lib32ncurses-dev
  libxml2-utils xsltproc
)
APT_OPTIONAL=(
  libx11-dev libgl-dev x11proto-dev
  libbz2-dev libreadline-dev libsqlite3-dev libffi-dev libssl-dev
  bc rsync cpio file lz4 xz-utils wget
  repo android-sdk-platform-tools-common android-sdk-libsparse-utils mkbootimg
)

if [[ -t 1 ]]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B='' G='' Y='' R='' N=''; fi
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
ok()   { printf '%s  ✓ %s%s\n' "$G" "$*" "$N"; }
warn() { printf '%s  ! %s%s\n' "$Y" "$*" "$N"; }
die()  { printf '%s  ✗ %s%s\n' "$R" "$*" "$N" >&2; exit 1; }
trap 'printf "%s  ✗ Failed at line %s: %s%s\n" "$R" "$LINENO" "$BASH_COMMAND" "$N" >&2; printf "    Logs: %s\n" "$LOGS" >&2' ERR

run_logged() {
  local log="$LOGS/$1.log"; shift
  if ! "$@" >"$log" 2>&1; then
    printf '%s  ✗ Failed, end of log %s:%s\n' "$R" "$log" "$N" >&2
    tail -n 30 "$log" >&2; return 1
  fi
}
download() {
  local url="$1" dest="$2"
  [[ -s "$dest" ]] && return 0
  curl -fL --retry 3 --retry-delay 3 --connect-timeout 20 -o "$dest.part" "$url"
  mv "$dest.part" "$dest"
}
check_sha256() { echo "$2  $1" | sha256sum -c --quiet - || die "Bad SHA-256: $1 (delete it and run again)"; }
check_md5()    { echo "$2  $1" | md5sum    -c --quiet - || die "Bad MD5: $1 (delete it and run again)"; }

# -std=gnu99 (C23 by default otherwise), -fcommon (GCC>=10), -fpermissive (GCC>=14)
legacy_cc() {
  local cc="gcc -std=gnu99 -fcommon"
  if echo 'int x;' | gcc -x c -std=gnu99 -fpermissive -Werror -c -o /dev/null - 2>/dev/null; then cc+=" -fpermissive"; fi
  printf '%s' "$cc"
}

preflight() {
  step "Checks"
  [[ $EUID -ne 0 ]] || die "Run this script as your normal user (it uses sudo when needed)."
  [[ "$(uname -m)" == x86_64 ]] || die "An x86_64 machine is required."
  command -v sudo >/dev/null || die "sudo is required."
  # shellcheck disable=SC1091
  [[ -r /etc/os-release ]] && . /etc/os-release && ok "${PRETTY_NAME:-?}"
  mkdir -p "$KK" "$SRC" "$BUILD" "$LOGS" "$KK/bin"
  local free_gb ram_gb
  free_gb=$(df -BG --output=avail "$KK" | tail -1 | tr -dc '0-9')
  ram_gb=$(awk '/MemTotal/ {print int($2/1024/1024)}' /proc/meminfo)
  if (( free_gb >= 100 )); then ok "Free space: ${free_gb} GB"; else warn "Free space: ${free_gb} GB, plan for at least 150 GB."; fi
  if (( ram_gb >= 15 )); then ok "RAM: ${ram_gb} GB"; else warn "RAM: ${ram_gb} GB, 16 GB recommended (or swap + fewer jobs)."; fi
}

install_apt() {
  step "apt packages"
  if dpkg -s "${APT_REQUIRED[@]}" >/dev/null 2>&1; then ok "already installed"; return; fi
  sudo apt-get update -qq
  local p missing_req=() avail=() missing_opt=()
  for p in "${APT_REQUIRED[@]}"; do if apt-cache show "$p" >/dev/null 2>&1; then avail+=("$p"); else missing_req+=("$p"); fi; done
  for p in "${APT_OPTIONAL[@]}"; do if apt-cache show "$p" >/dev/null 2>&1; then avail+=("$p"); else missing_opt+=("$p"); fi; done
  (( ${#missing_req[@]} == 0 )) || die "Required packages not found: ${missing_req[*]}"
  sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${avail[@]}"
  ok "${#avail[@]} packages installed"
  (( ${#missing_opt[@]} == 0 )) || warn "Optional packages missing (not blocking): ${missing_opt[*]}"
}

patch_make_glob() {
  local f="$1/glob/glob.c"
  perl -0pi -e 's/#if !defined __alloca && !defined __GNU_LIBRARY__\n/#if !defined __alloca\n/;
               s/#ifndef __GNU_LIBRARY__\n# define __stat stat\n/#ifndef __stat\n# define __stat stat\n/' "$f"
  grep -q '^#if !defined __alloca$' "$f" && grep -q '^#ifndef __stat$' "$f" || die "The glob.c fix did not apply."
}
build_make() {
  local srcdir="$1" cc; cc="$(legacy_cc)"
  patch_make_glob "$srcdir"
  ( cd "$srcdir"
    run_logged make-configure env CC="$cc" ./configure --prefix="$KK/make-3.82" --disable-nls
    run_logged make-glob make -C glob -j"$JOBS"
    run_logged make-build make make -j"$JOBS" )
  install -Dm755 "$srcdir/make" "$KK/make-3.82/bin/make"
  ln -sf "$KK/make-3.82/bin/make" "$KK/bin/make"
}
install_make() {
  step "GNU make 3.82"
  if [[ -x "$KK/make-3.82/bin/make" ]] && "$KK/make-3.82/bin/make" --version | grep -q 'GNU Make 3.82'; then ok "already installed"; return; fi
  download "$MAKE_URL" "$SRC/make-3.82.tar.gz"
  download "$MAKE_URL.sig" "$SRC/make-3.82.tar.gz.sig"
  download "$GNU_KEYRING_URL" "$SRC/gnu-keyring.gpg"
  if gpgv --keyring "$SRC/gnu-keyring.gpg" "$SRC/make-3.82.tar.gz.sig" "$SRC/make-3.82.tar.gz" >"$LOGS/make-gpg.log" 2>&1; then ok "GPG signature verified"
  elif grep -qi 'bad signature' "$LOGS/make-gpg.log"; then die "INVALID GPG signature of make-3.82 (see $LOGS/make-gpg.log)."
  else warn "This gpgv cannot check the signature (see $LOGS/make-gpg.log), going on."; fi
  rm -rf "$BUILD/make-3.82"; tar -xzf "$SRC/make-3.82.tar.gz" -C "$BUILD"
  build_make "$BUILD/make-3.82"
  ok "$("$KK/bin/make" --version | head -1)"
}

build_python() {
  local srcdir="$1" cc; cc="$(legacy_cc)"
  ( cd "$srcdir"
    run_logged python-configure env CC="$cc" ./configure --prefix="$KK/python2.7" --without-ensurepip
    run_logged python-build make -j"$JOBS"
    run_logged python-install make install )
  local b; for b in python python2 python2.7; do ln -sf "$KK/python2.7/bin/python2.7" "$KK/bin/$b"; done
}
install_python2() {
  step "Python 2.7.18"
  if [[ -x "$KK/python2.7/bin/python2.7" ]] && "$KK/python2.7/bin/python2.7" -V 2>&1 | grep -q '2.7.18'; then ok "already installed"; return; fi
  download "$PY_URL" "$SRC/Python-2.7.18.tgz"
  check_md5 "$SRC/Python-2.7.18.tgz" "$PY_MD5"
  rm -rf "$BUILD/Python-2.7.18"; tar -xzf "$SRC/Python-2.7.18.tgz" -C "$BUILD"
  build_python "$BUILD/Python-2.7.18"
  "$KK/bin/python" -c 'import zlib, zipfile, subprocess, xml.dom.minidom' || die "Incomplete Python 2."
  ok "$("$KK/bin/python" -V 2>&1)"
}

# KitKat rejects "OpenJDK" in java -version: a wrapper presents Zulu 6 as Java 1.6.
install_java_wrapper() {
  local jdk="$1"
  [[ -x "$jdk/bin/java-zulu" ]] || mv "$jdk/bin/java" "$jdk/bin/java-zulu"
  cat >"$jdk/bin/java" <<'EOF'
#!/bin/sh
REAL="$(dirname "$(readlink -f "$0")")/java-zulu"
if [ "$#" -eq 1 ] && [ "$1" = "-version" ]; then
  "$REAL" -version 2>&1 | sed -e '1s/^openjdk version/java version/' -e 's/[Oo]pen[Jj][Dd][Kk]/Java(TM) SE/g' >&2
  exit 0
fi
exec "$REAL" "$@"
EOF
  chmod 755 "$jdk/bin/java"
}
install_jdk6() {
  step "JDK 6 (Azul Zulu 6)"
  if [[ -x "$KK/jdk6/bin/java-zulu" && -f "$KK/jdk6/lib/tools.jar" ]]; then ok "already installed"; return; fi
  download "$ZULU_URL" "$SRC/zulu6.tar.gz"
  check_sha256 "$SRC/zulu6.tar.gz" "$ZULU_SHA256"
  rm -rf "$BUILD/jdk6" "$KK/jdk6"; mkdir -p "$BUILD/jdk6"
  tar -xzf "$SRC/zulu6.tar.gz" -C "$BUILD/jdk6"
  local top; top="$(find "$BUILD/jdk6" -mindepth 1 -maxdepth 1 -type d | head -1)"
  mv "$top" "$KK/jdk6"
  [[ -f "$KK/jdk6/lib/tools.jar" ]] || die "tools.jar not found in the JDK."
  install_java_wrapper "$KK/jdk6"
  ok "Zulu 6 installed in $KK/jdk6"
}

extract_ncurses() {
  local debdir="$1" d
  rm -rf "$KK/compat"; mkdir -p "$KK/compat"
  for d in "$debdir"/*.deb; do dpkg-deb -x "$d" "$KK/compat"; done
  [[ -e "$KK/compat/lib/x86_64-linux-gnu/libtinfo.so.5" && -e "$KK/compat/lib32/libtinfo.so.5" ]] || die "ncurses 5 missing after extraction."
}
install_ncurses5() {
  step "ncurses 5 compatibility"
  if [[ -e "$KK/compat/lib/x86_64-linux-gnu/libncurses.so.5" && -e "$KK/compat/lib32/libncurses.so.5" ]]; then ok "already installed"; return; fi
  local entry path sha name dir="$SRC/ncurses5"; mkdir -p "$dir"
  for entry in "${NCURSES_DEBS[@]}"; do
    path="${entry%%|*}"; sha="${entry##*|}"; name="${path##*/}"
    if [[ ! -s "$dir/$name" ]] && ! curl -fsSL --retry 3 -o "$dir/$name" "$UBUNTU_POOL/$path"; then
      local pkg="${name%%_*}" series="${name#*_}" latest; series="${series%%-*}"
      latest=$(curl -fsSL "$UBUNTU_POOL/${path%/*}/" | grep -o "${pkg}_${series}[^\"]*_amd64\.deb" | sort -uV | tail -1) || die "Cannot find $pkg"
      warn "$name is no longer on the mirror, using $latest (checksum not verified)"
      rm -f "$dir/$name"; curl -fsSL --retry 3 -o "$dir/$latest" "$UBUNTU_POOL/${path%/*}/$latest"; continue
    fi
    check_sha256 "$dir/$name" "$sha"
  done
  extract_ncurses "$dir"
  ok "libncurses.so.5 / libtinfo.so.5 (64 and 32-bit) in $KK/compat"
}

install_platform_tools() {
  step "Google platform-tools (adb, fastboot)"
  download "$PLATFORM_TOOLS_URL" "$SRC/platform-tools-latest-linux.zip"
  rm -rf "$KK/platform-tools"; unzip -q "$SRC/platform-tools-latest-linux.zip" -d "$KK"
  ok "$("$KK/platform-tools/fastboot" --version | head -1)"
  if getent group plugdev >/dev/null && ! id -nG "$USER" | grep -qw plugdev; then
    sudo usermod -aG plugdev "$USER"; warn "Added to the plugdev group: log in again to use fastboot without sudo."
  fi
}

install_repo() {
  step "repo"
  local launcher
  if [[ -x /usr/bin/repo ]]; then launcher=/usr/bin/repo
  else mkdir -p "$KK/repo-launcher"; download "$REPO_LAUNCHER_URL" "$KK/repo-launcher/repo"; launcher="$KK/repo-launcher/repo"; fi
  printf '#!/bin/sh\nexec python3 %q "$@"\n' "$launcher" >"$KK/bin/repo"; chmod 755 "$KK/bin/repo"
  ok "repo → $launcher"
}

write_env() {
  step "Environment file"
  cat >"$KK/env.sh" <<EOF
# KitKat build environment: source it BEFORE "source build/envsetup.sh"
export KK_TOOLS="$KK"
export JAVA_HOME="\$KK_TOOLS/jdk6"
export ANDROID_JAVA_HOME="\$JAVA_HOME"
export PATH="\$KK_TOOLS/bin:\$JAVA_HOME/bin:\$KK_TOOLS/platform-tools:\$PATH"
export LD_LIBRARY_PATH="\$KK_TOOLS/compat/lib/x86_64-linux-gnu:\$KK_TOOLS/compat/lib32\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export LC_ALL=C
case "\$-" in *i*) PS1="(kk) \${PS1:-\\\$ }" ;; esac
EOF
  ok "$KK/env.sh"
}

verify() {
  step "Final check (the tests of build/core/main.mk)"
  (
    # shellcheck disable=SC1091
    source "$KK/env.sh"
    local mv jv jcv fails=0
    mv="$(make --version | head -1)"
    if [[ "$mv" == "GNU Make 3.82" ]]; then ok "make: $mv"; else warn "make: $mv"; fails=1; fi
    if java -version 2>&1 | grep -qi openjdk; then warn "java -version still mentions OpenJDK"; fails=1; fi
    jv="$(java -version 2>&1 | head -n 1)"
    if echo "$jv" | grep -q '^java .*[ "]1\.6[\. "$]'; then ok "java: $jv"; else warn "java: $jv"; fails=1; fi
    jcv="$(javac -version 2>&1 | head -n 1)"
    if echo "$jcv" | grep -q '[ "]1\.6[\. "$]'; then ok "javac: $jcv"; else warn "javac: $jcv"; fails=1; fi
    ok "python: $(python -V 2>&1), python3: $(python3 -V 2>&1)"
    ok "repo: $(command -v repo), fastboot: $(command -v fastboot)"
    (( fails == 0 )) || die "At least one check failed."
  )
}

main() {
  local t0=$SECONDS
  preflight; install_apt; install_make; install_python2; install_jdk6
  install_ncurses5; install_platform_tools; install_repo; write_env; verify
  step "Done in $(( (SECONDS - t0) / 60 )) min. scripts/build.sh sources $KK/env.sh by itself."
}
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then main "$@"; fi
