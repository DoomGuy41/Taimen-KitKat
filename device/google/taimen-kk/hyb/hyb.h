/* libhyb: loads Android 11 libraries (ARM 32-bit) into a KitKat process.
 * Their libc/libm/libdl/liblog/libcutils/libz imports resolve to KitKat's libraries, completed by
 * hooks (hooks.c); the other dependencies (libc++, libutils, Adreno driver, QTI gralloc...) are
 * loaded by libhyb itself from the Android 11 root. */
#ifndef HYB_H
#define HYB_H
#ifdef __cplusplus
extern "C" {
#endif

/* root of the Android 11 files (default /system/a11): <root>/vendor/lib, <root>/system/lib */
void hyb_set_root(const char *root);
void *hyb_dlopen(const char *name, int flags);
void *hyb_dlsym(void *handle, const char *symbol);
int hyb_dlclose(void *handle);
const char *hyb_dlerror(void);

#ifdef __cplusplus
}
#endif
#endif
