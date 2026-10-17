// ported from pdx203-jellybean by @chardidathing, https://github.com/chardidathing/pdx203-jellybean
// static helper, kitkat's toolbox has no chroot and its mount can't bind.
// usage: hwchroot <root> <program> [args...]
// mounts live in a private namespace so nothing leaks back into kitkat.
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIB_PATH "/vendor/lib/egl:/vendor/lib:/system/lib:/vendor/lib64/egl:/vendor/lib64:/system/lib64"

static int bind_into(const char *root, const char *src, const char *dst) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s", root, dst);
    mkdir(path, 0755);
    if (mount(src, path, NULL, MS_BIND | MS_REC, NULL)) {
        fprintf(stderr, "hwchroot: bind %s -> %s: %s\n", src, path, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <root> <program> [args...]\n", argv[0]);
        return 1;
    }
    const char *root = argv[1];
    if (unshare(CLONE_NEWNS)) { perror("hwchroot: unshare"); return 1; }
    // kitkat init leaves / shared-less, but be explicit so our binds never propagate
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    // /dev wholesale, it has kgsl-3d0, ion and (deliberately) no hwbinder
    if (bind_into(root, "/dev", "/dev")) return 1;
    if (bind_into(root, "/proc", "/proc")) return 1;
    if (bind_into(root, "/sys", "/sys")) return 1;

    if (chroot(root) || chdir("/")) { perror("hwchroot: chroot"); return 1; }

    // kitkat's ANDROID_PROPERTY_WORKSPACE means nothing to modern bionic, don't pass it on
    char *envp[] = {"PATH=/system/bin", "LD_LIBRARY_PATH=" LIB_PATH, "HOME=/", NULL};
    execve(argv[2], &argv[2], envp);
    perror("hwchroot: execve");
    return 1;
}
