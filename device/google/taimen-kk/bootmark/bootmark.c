/* bootmark, in the Android 11 chroot (arm64). Tells the bootloader that the current slot booted
 * (markBootSuccessful of the Android 11 bootctrl.msm8998 HAL). Without it, every KitKat boot
 * uses up a retry and the bootloader eventually switches to the other slot.
 * The HAL reads ro.boot.slot_suffix and ro.boot.bootdevice through property_get: the executable
 * provides that symbol (exported, so it wins the global lookup) from /proc/cmdline.
 * usage: bootmark [anything: only print the slot status] */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

struct hw_module_t {
    unsigned tag; unsigned short module_api_version, hal_api_version;
    const char *id, *name, *author; void *methods, *dso; unsigned long reserved[32 - 7];
};
struct boot_control_module {
    struct hw_module_t common;
    void (*init)(struct boot_control_module *);
    unsigned (*getNumberSlots)(struct boot_control_module *);
    unsigned (*getCurrentSlot)(struct boot_control_module *);
    int (*markBootSuccessful)(struct boot_control_module *);
    int (*setActiveBootSlot)(struct boot_control_module *, unsigned);
    int (*setSlotAsUnbootable)(struct boot_control_module *, unsigned);
    int (*isSlotBootable)(struct boot_control_module *, unsigned);
    const char *(*getSuffix)(struct boot_control_module *, unsigned);
    int (*isSlotMarkedSuccessful)(struct boot_control_module *, unsigned);
};

__attribute__((visibility("default"))) int property_get(const char *key, char *value, const char *def)
{
    value[0] = 0;
    if (!strncmp(key, "ro.boot.", 8)) {  /* ro.boot.X = androidboot.X of the command line */
        char buf[4096] = "", pat[128];
        FILE *f = fopen("/proc/cmdline", "r");
        if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
        snprintf(pat, sizeof(pat), " androidboot.%s=", key + 8);
        char *p = strstr(buf, pat);
        if (p) sscanf(p + strlen(pat), "%91s", value);
    }
    if (!value[0] && def) snprintf(value, 92, "%s", def);
    return strlen(value);
}

int main(int argc, char **argv)
{
    void *so = dlopen("/vendor/lib64/hw/bootctrl.msm8998.so", RTLD_NOW);
    if (!so) { fprintf(stderr, "bootmark: %s\n", dlerror()); return 1; }
    struct boot_control_module *m = dlsym(so, "HMI");
    if (m->init) m->init(m);
    unsigned cur = m->getCurrentSlot(m);
    if (argc < 2) {
        int r = m->markBootSuccessful(m);
        printf("bootmark: slot %s marked successful: %d\n", m->getSuffix(m, cur), r);
    }
    for (unsigned s = 0; s < m->getNumberSlots(m); s++)
        printf("bootmark: slot %s%s bootable=%d successful=%d\n", m->getSuffix(m, s), s == cur ? " (current)" : "",
               m->isSlotBootable(m, s), m->isSlotMarkedSuccessful(m, s));
    return 0;
}
