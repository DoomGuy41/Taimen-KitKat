/* camtest: loads the Android 11 Qualcomm camera HAL (camera.msm8998.so) through libhyb into a
 * KitKat process and prints what it reports (cameras, versions, static metadata).
 * usage: camtest [Android 11 root] [open] (with "open", also opens and closes every camera) */
#include <hardware/hardware.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <asm/sigcontext.h>
#include <dlfcn.h>
#include "hyb.h"
int hyb_dladdr(const void *addr, Dl_info *info);

/* Android 11 camera_module_t and camera_info (hardware/camera_common.h, 32-bit) */
struct a11_camera_info {
    int facing;
    int orientation;
    uint32_t device_version;
    const void *static_camera_characteristics;
    int resource_cost;
    char **conflicting_devices;
    size_t conflicting_devices_length;
};
struct a11_camera_module {
    struct hw_module_t common;
    int (*get_number_of_cameras)(void);
    int (*get_camera_info)(int id, struct a11_camera_info *info);
    int (*set_callbacks)(const void *callbacks);
    void (*get_vendor_tag_ops)(void *ops);
    int (*open_legacy)(const struct hw_module_t *m, const char *id, uint32_t v, struct hw_device_t **d);
    int (*set_torch_mode)(const char *id, int enabled);
    int (*init)(void);
    void *reserved[5];
};
struct a11_module_callbacks {
    void (*camera_device_status_change)(const void *cb, int id, int status);
    void (*torch_mode_status_change)(const void *cb, const char *id, int status);
};
static void dev_status(const void *cb, int id, int st) { printf("  callback: camera %d status %d\n", id, st); }
static void torch_status(const void *cb, const char *id, int st) { printf("  callback: torch %s status %d\n", id, st); }
static const struct a11_module_callbacks s_cbs = { dev_status, torch_status };

struct entry {
    size_t index;
    uint32_t tag;
    uint8_t type;
    size_t count;
    union { uint8_t *u8; int32_t *i32; float *f; int64_t *i64; double *d; void *p; } data;
};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* CAMTEST_WATCH: the table page is made read-only, the first writer is reported */
static void on_segv(int sig, siginfo_t *si, void *ctx)
{
    struct { unsigned long flags; void *link; stack_t stack; struct sigcontext mc; } *uc = ctx;
    uintptr_t pc = uc->mc.arm_pc, lr = uc->mc.arm_lr;
    Dl_info a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    hyb_dladdr((void *)pc, &a);
    hyb_dladdr((void *)lr, &b);
    printf("WRITE at %p: pc %p (%s+%#x, %s), lr %p (%s+%#x)\n", si->si_addr, (void *)pc,
           a.dli_fname ? a.dli_fname : "?", (unsigned)(pc - (uintptr_t)a.dli_fbase), a.dli_sname ? a.dli_sname : "?",
           (void *)lr, b.dli_fname ? b.dli_fname : "?", (unsigned)(lr - (uintptr_t)b.dli_fbase));
    _exit(3);
}

static void dbgtable(const char *when)
{
    if (!getenv("CAMTEST_DBGTABLE"))  /* diagnostics: cam_loginfo table of libmmcamera_interface */
        return;
    void *mi = hyb_dlopen("libmmcamera_interface.so", 0);
    uintptr_t f = (uintptr_t)hyb_dlsym(mi, "mm_camera_set_dbg_log_properties");
    uintptr_t base = (f & ~1u) - 0x116d4;
    uint32_t *t = (uint32_t *)(base + strtoul(getenv("CAMTEST_DBGTABLE"), NULL, 0));
    printf("%s: base %p, table %p\n", when, (void *)base, (void *)t);
    for (int i = 0; i < 5; i++)
        printf("  %2d: %08x %08x %08x %08x\n", i, t[4 * i], t[4 * i + 1], t[4 * i + 2], t[4 * i + 3]);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGHUP, SIG_IGN);  /* survive the end of the adb session (debugging) */
    double t0 = now();
    int open_cams = 0;
    hyb_set_root("/system/a11");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/')
            hyb_set_root(argv[i]);
        else if (!strcmp(argv[i], "open"))
            open_cams = 1;
    }
    void *so = hyb_dlopen("/vendor/lib/hw/camera.msm8998.so", 0);
    if (!so) {
        printf("camera.msm8998: %s\n", hyb_dlerror());
        return 1;
    }
    struct a11_camera_module *m = hyb_dlsym(so, "HMI");
    dbgtable("after loading");
    void *md = hyb_dlopen("libcamera_metadata.so", 0);
    size_t (*count)(const void *) = hyb_dlsym(md, "get_camera_metadata_entry_count");
    int (*find)(const void *, uint32_t, struct entry *) = hyb_dlsym(md, "find_camera_metadata_ro_entry");
    const char *(*tagname)(uint32_t) = hyb_dlsym(md, "get_camera_metadata_tag_name");
    if (!m || !count || !find) {
        printf("missing symbols\n");
        return 1;
    }
    printf("loaded in %.0f ms: %s (%s), module API %d.%d\n", (now() - t0) * 1000, m->common.name,
           m->common.author, m->common.module_api_version >> 8, m->common.module_api_version & 0xff);
    dbgtable("before init");
    if (getenv("CAMTEST_PAUSE")) {  /* wait for a debugger (gdbserver --attach) */
        void *mi = hyb_dlopen("libmmcamera_interface.so", 0);
        uintptr_t f = (uintptr_t)hyb_dlsym(mi, "mm_camera_set_dbg_log_properties");
        signal(SIGHUP, SIG_IGN);
        unlink("/data/local/tmp/ct.go");
        printf("pid %d table %p\n", getpid(), (void *)((f & ~1u) - 0x116d4 + 0x23b74));
        while (access("/data/local/tmp/ct.go", F_OK))
            usleep(100000);
    }
    if (getenv("CAMTEST_WATCH")) {
        void *mi = hyb_dlopen("libmmcamera_interface.so", 0);
        uintptr_t f = (uintptr_t)hyb_dlsym(mi, "mm_camera_set_dbg_log_properties");
        uintptr_t page = ((f & ~1u) - 0x116d4 + 0x23000);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        printf("watched page %p: %d\n", (void *)page, mprotect((void *)page, 4096, PROT_READ));
    }
    if (m->init) {
        t0 = now();
        printf("init: %d (%.0f ms)\n", m->init(), (now() - t0) * 1000);
    }
    t0 = now();
    int n = m->get_number_of_cameras();  /* creates the QCamera2 factory: before set_callbacks */
    printf("%d camera(s) (%.0f ms)\n", n, (now() - t0) * 1000);
    dbgtable("after get_number_of_cameras");
    if (m->set_callbacks)
        printf("set_callbacks: %d\n", m->set_callbacks(&s_cbs));
    static const uint32_t keys[] = { 0xD0001, 0xD0002, 0xD0003, 0xD0004, 0xD0005, 0xD0006, 0xD0007, 0xD0008,
                                     0xD000A, 0xD000B, 0xD000C, 0xD000D, 0xE0001, 0x150000 };
    for (int i = 0; i < n; i++) {
        struct a11_camera_info ci;
        memset(&ci, 0, sizeof(ci));
        int r = m->get_camera_info(i, &ci);
        printf("camera %d: r=%d, %s, orientation %d, device %d.%d, %zu static entries\n", i, r,
               ci.facing == 0 ? "back" : "front", ci.orientation, (ci.device_version >> 8) & 0xff,
               ci.device_version & 0xff, ci.static_camera_characteristics ? count(ci.static_camera_characteristics) : 0);
        for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            struct entry e;
            if (find(ci.static_camera_characteristics, keys[k], &e) == 0)
                printf("    %-45s %zu values\n", tagname(keys[k]), e.count);
        }
        if (open_cams) {
            char id[4];
            snprintf(id, sizeof(id), "%d", i);
            struct hw_device_t *d = NULL;
            t0 = now();
            r = m->common.methods->open(&m->common, id, &d);
            printf("  open: %d in %.0f ms, device version 0x%x\n", r, (now() - t0) * 1000, d ? d->version : 0);
            if (d)
                printf("  close: %d\n", d->close(d));
        }
    }
    return 0;
}
