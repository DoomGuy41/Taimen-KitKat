/* GLES shim (KitKat's libGLESv1_CM_adreno.so or libGLESv2_adreno.so). Loads the real Android 11
 * Adreno library through libhyb and fills the jump table (gles*_stubs.S). */
#define LOG_TAG "gles_adreno"
#include <cutils/log.h>
#include "../hyb/hyb.h"

#ifdef SHIM_GLES1
#define LIB "libGLESv1_CM_adreno.so"
#define TABLE gles1_table
#define NAMES gles1_table_names
#else
#define LIB "libGLESv2_adreno.so"
#define TABLE gles2_table
#define NAMES gles2_table_names
#endif

extern const char *const NAMES[];
extern void *TABLE[];

static void gl_missing(void)
{
    ALOGE("called a GL function missing from the driver");
}

__attribute__((constructor)) static void shim_init(void)
{
    hyb_set_root("/system/a11");
    void *so = hyb_dlopen(LIB, 0);
    if (!so) {
        ALOGE("%s: %s", LIB, hyb_dlerror());
        return;
    }
    int n = 0, missing = 0;
    for (; NAMES[n]; n++) {
        TABLE[n] = hyb_dlsym(so, NAMES[n]);
        if (!TABLE[n]) {
            TABLE[n] = (void *)gl_missing;
            missing++;
        }
    }
    ALOGI("%s: %d functions (%d missing)", LIB, n, missing);
}
