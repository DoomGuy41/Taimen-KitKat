/* EGL shim (KitKat's libEGL_adreno.so) in front of the real Android 11 libEGL_adreno loaded by
 * libhyb. KitKat's EGL loader takes eglGetProcAddress and the few intercepted functions from
 * here; everything else comes straight from the driver.
 *
 * Interceptions:
 *  - windows (eglCreateWindowSurface): an intermediate ANativeWindow translates the Android 11
 *    query/perform codes KitKat does not know, and gives the driver buffers in the Android 11
 *    format (layerCount, 64-bit usage) whose handle is the one imported into the QTI gralloc1;
 *  - EGLImages of native buffers (eglCreateImageKHR, EGL_NATIVE_BUFFER_ANDROID): same
 *    intermediate buffer. */
#define LOG_TAG "egl_adreno"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <system/window.h>
#include "../hyb/hyb.h"

typedef void (*fn_t)(void);
static void *s_egl, *s_gles2;
static fn_t (*r_getproc)(const char *);
static EGLSurface (*r_create_window)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLBoolean (*r_destroy_surface)(EGLDisplay, EGLSurface);
static EGLImageKHR (*r_create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
static buffer_handle_t (*s_import)(buffer_handle_t);

/* ------------------------------------------------------------------ buffers in A11 format */
struct a11_buf {
    struct android_native_base_t common;
    int width, height, stride, format, usage_deprecated;
    uintptr_t layerCount;
    void *reserved[1];
    const native_handle_t *handle;
    uint64_t usage;
    void *reserved_proc[6];
    /* shim's own fields */
    ANativeWindowBuffer_t *orig;
    struct a11_buf *next;
};

#define NB 512
static struct a11_buf *s_bufs[NB];
static pthread_mutex_t s_buf_lock = PTHREAD_MUTEX_INITIALIZER;

static void buf_inc(struct android_native_base_t *b)
{
    ANativeWindowBuffer_t *o = ((struct a11_buf *)b)->orig;
    o->common.incRef(&o->common);
}

static void buf_dec(struct android_native_base_t *b)
{
    ANativeWindowBuffer_t *o = ((struct a11_buf *)b)->orig;
    o->common.decRef(&o->common);
}

/* intermediate buffer of a KitKat buffer, refreshed when the handle changed */
static struct a11_buf *wrap(ANativeWindowBuffer_t *b)
{
    if (!b || !s_import)
        return NULL;
    buffer_handle_t q = s_import(b->handle);
    if (!q) {
        ALOGE("buffer %p: handle cannot be imported into the QTI gralloc1", b);
        return NULL;
    }
    unsigned k = ((uintptr_t)b >> 4) % NB;
    pthread_mutex_lock(&s_buf_lock);
    struct a11_buf *w = s_bufs[k];
    while (w && w->orig != b)
        w = w->next;
    if (!w) {
        w = calloc(1, sizeof(*w));
        w->orig = b;
        w->next = s_bufs[k];
        s_bufs[k] = w;
    }
    w->common.magic = b->common.magic;
    w->common.version = sizeof(ANativeWindowBuffer_t);
    w->common.incRef = buf_inc;
    w->common.decRef = buf_dec;
    w->width = b->width;
    w->height = b->height;
    w->stride = b->stride;
    w->format = b->format;
    w->usage_deprecated = b->usage;
    w->layerCount = 1;
    w->handle = q;
    w->usage = (uint32_t)b->usage;
    pthread_mutex_unlock(&s_buf_lock);
    return w;
}

static ANativeWindowBuffer_t *orig(ANativeWindowBuffer_t *b)
{
    return b ? ((struct a11_buf *)b)->orig : NULL;
}

/* ------------------------------------------------------------------ intermediate window */
struct a11_win {
    ANativeWindow w;
    ANativeWindow *real;
    EGLSurface surface;
    struct a11_win *next;
};
static struct a11_win *s_wins;
static pthread_mutex_t s_win_lock = PTHREAD_MUTEX_INITIALIZER;
#define REAL(win) (((struct a11_win *)(win))->real)

static void win_inc(struct android_native_base_t *b)
{
    ANativeWindow *r = ((struct a11_win *)b)->real;
    r->common.incRef(&r->common);
}

static void win_dec(struct android_native_base_t *b)
{
    ANativeWindow *r = ((struct a11_win *)b)->real;
    r->common.decRef(&r->common);
}

static int w_swap_interval(ANativeWindow *win, int i) { return REAL(win)->setSwapInterval(REAL(win), i); }

static int w_dequeue(ANativeWindow *win, ANativeWindowBuffer_t **buf, int *fence)
{
    ANativeWindowBuffer_t *kb = NULL;
    int r = REAL(win)->dequeueBuffer(REAL(win), &kb, fence);
    if (r)
        return r;
    struct a11_buf *w = wrap(kb);
    if (!w) {
        REAL(win)->cancelBuffer(REAL(win), kb, *fence);
        *fence = -1;
        return -EINVAL;
    }
    *buf = (ANativeWindowBuffer_t *)w;
    return 0;
}

static int w_queue(ANativeWindow *win, ANativeWindowBuffer_t *buf, int fence)
{
    return REAL(win)->queueBuffer(REAL(win), orig(buf), fence);
}

static int w_cancel(ANativeWindow *win, ANativeWindowBuffer_t *buf, int fence)
{
    return REAL(win)->cancelBuffer(REAL(win), orig(buf), fence);
}

static int w_dequeue_dep(ANativeWindow *win, ANativeWindowBuffer_t **buf)
{
    ANativeWindowBuffer_t *kb = NULL;
    int r = REAL(win)->dequeueBuffer_DEPRECATED(REAL(win), &kb);
    if (r)
        return r;
    *buf = (ANativeWindowBuffer_t *)wrap(kb);
    return *buf ? 0 : -EINVAL;
}

static int w_lock_dep(ANativeWindow *win, ANativeWindowBuffer_t *buf)
{
    return REAL(win)->lockBuffer_DEPRECATED(REAL(win), orig(buf));
}

static int w_queue_dep(ANativeWindow *win, ANativeWindowBuffer_t *buf)
{
    return REAL(win)->queueBuffer_DEPRECATED(REAL(win), orig(buf));
}

static int w_cancel_dep(ANativeWindow *win, ANativeWindowBuffer_t *buf)
{
    return REAL(win)->cancelBuffer_DEPRECATED(REAL(win), orig(buf));
}

static int s_trace;  /* property debug.egl_shim.trace=1: log the driver's query/perform calls */

/* Android 11 queries beyond NATIVE_WINDOW_CONSUMER_USAGE_BITS (10) */
static int w_query(const ANativeWindow *win, int what, int *value)
{
    if (what == NATIVE_WINDOW_TRANSFORM_HINT) {
        /* no pre-rotation: with a 90 degree hint, the driver allocates buffers with swapped
         * dimensions and asks for the inverse rotation, two performs KitKat cannot follow
         * (see w_perform). SurfaceFlinger composes everything with GL, rotation is free there. */
        *value = 0;
        return 0;
    }
    if (what <= NATIVE_WINDOW_CONSUMER_USAGE_BITS) {
        int r = REAL(win)->query(REAL(win), what, value);
        if (s_trace)
            ALOGI("query %d -> %d (%d)", what, *value, r);
        return r;
    }
    switch (what) {
    case 11: /* STICKY_TRANSFORM */
    case 12: /* DEFAULT_DATASPACE */
    case 13: /* BUFFER_AGE: unknown, the driver redraws everything */
    case 14: case 15: /* dequeue/queue durations */
    case 18: /* FRAME_TIMESTAMPS_SUPPORTS_PRESENT */
    case 19: /* CONSUMER_IS_PROTECTED */
    case 20: /* DATASPACE */
        *value = 0;
        return 0;
    case 16: /* LAYER_COUNT */
    case 17: /* IS_VALID */
        *value = 1;
        return 0;
    case 21: /* MAX_BUFFER_COUNT */
        *value = 64;
        return 0;
    }
    return -EINVAL;
}

/* Android 11 perform operations: 0..16 same as KitKat, the others translated or ignored */
static int w_perform(ANativeWindow *win, int op, ...)
{
    ANativeWindow *r = REAL(win);
    va_list ap;
    va_start(ap, op);
    int ret = 0;
    if (s_trace) {
        va_list t;
        va_copy(t, ap);
        int a = va_arg(t, int), b = va_arg(t, int);
        va_end(t);
        ALOGI("perform %d (%d, %d)", op, a, b);
    }
    switch (op) {
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
        ret = r->perform(r, op, va_arg(ap, int));
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        ret = r->perform(r, op, va_arg(ap, size_t));
        break;
    case NATIVE_WINDOW_SET_CROP:
    case NATIVE_WINDOW_SET_POST_TRANSFORM_CROP:
        ret = r->perform(r, op, va_arg(ap, void *));
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY: {
        int w = va_arg(ap, int), h = va_arg(ap, int), f = va_arg(ap, int);
        ret = r->perform(r, op, w, h, f);
        break;
    }
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
        /* the driver resets the transform to 0 (no pre-rotation): on KitKat this would cancel
         * the one requested by the app (native_window_set_buffers_transform) */
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
        /* the Android 11 driver enforces the default window size; on KitKat this would
         * overwrite the size requested by the app (ANativeWindow_setBuffersGeometry sets these
         * same dimensions there, whereas Android 11 uses the "user" dimensions). Without a
         * request from the app, KitKat already follows the default size: ignored. */
        break;
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS: {
        int w = va_arg(ap, int), h = va_arg(ap, int);
        ret = r->perform(r, op, w, h);
        break;
    }
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
        ret = r->perform(r, op, va_arg(ap, int64_t));
        break;
    case NATIVE_WINDOW_LOCK: {
        void *b = va_arg(ap, void *), *rect = va_arg(ap, void *);
        ret = r->perform(r, op, b, rect);
        break;
    }
    case NATIVE_WINDOW_UNLOCK_AND_POST:
        ret = r->perform(r, op);
        break;
    /* Android 11 numbers (libs/nativewindow/include/system/window.h) */
    case 30: /* SET_USAGE64 */
        ret = r->perform(r, NATIVE_WINDOW_SET_USAGE, (int)va_arg(ap, uint64_t));
        break;
    case 31: { /* GET_CONSUMER_USAGE64 */
        uint64_t *out = va_arg(ap, uint64_t *);
        int u = 0;
        ret = r->query(r, NATIVE_WINDOW_CONSUMER_USAGE_BITS, &u);
        *out = (uint32_t)u;
        break;
    }
    case 23: /* GET_REFRESH_CYCLE_DURATION */
        *va_arg(ap, int64_t *) = 16666667;
        break;
    case 24: { /* GET_NEXT_FRAME_ID */
        static uint64_t id;
        *va_arg(ap, uint64_t *) = ++id;
        break;
    }
    case 28: /* GET_WIDE_COLOR_SUPPORT */
    case 29: /* GET_HDR_SUPPORT */
        *va_arg(ap, uint8_t *) = 0;  /* bool* */
        break;
    case 36: /* GET_LAST_DEQUEUE_START */
    case 38: /* GET_LAST_DEQUEUE_DURATION */
    case 39: /* GET_LAST_QUEUE_DURATION */
        *va_arg(ap, int64_t *) = 0;
        break;
    case 21: /* SET_SHARED_BUFFER_MODE */
        ret = va_arg(ap, int) ? -EINVAL : 0;
        break;
    case 17: case 18: case 19: case 20: case 22: case 25: case 32: case 33: case 34: case 35:
    case 37: case 40:
        /* sticky transform, sideband, dataspace, damage, auto refresh, timestamps, HDR
         * metadata, pre-rotation, dequeue timeout, frame rate: not applicable on KitKat */
        break;
    default:
        ALOGW("unhandled Android 11 perform %d", op);
        ret = -EINVAL;
    }
    va_end(ap);
    return ret;
}

static struct a11_win *wrap_window(ANativeWindow *real)
{
    struct a11_win *w = calloc(1, sizeof(*w));
    memcpy(&w->w, real, sizeof(ANativeWindow));  /* const fields (flags, intervals, dpi) */
    w->real = real;
    w->w.common.magic = real->common.magic;
    w->w.common.version = sizeof(ANativeWindow);
    w->w.common.incRef = win_inc;
    w->w.common.decRef = win_dec;
    w->w.setSwapInterval = w_swap_interval;
    w->w.dequeueBuffer_DEPRECATED = w_dequeue_dep;
    w->w.lockBuffer_DEPRECATED = w_lock_dep;
    w->w.queueBuffer_DEPRECATED = w_queue_dep;
    w->w.query = w_query;
    w->w.perform = w_perform;
    w->w.cancelBuffer_DEPRECATED = w_cancel_dep;
    w->w.dequeueBuffer = w_dequeue;
    w->w.queueBuffer = w_queue;
    w->w.cancelBuffer = w_cancel;
    return w;
}

/* ------------------------------------------------------------------ intercepted EGL calls */
EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win, const EGLint *attr)
{
    struct a11_win *w = wrap_window((ANativeWindow *)win);
    EGLSurface s = r_create_window(dpy, cfg, (EGLNativeWindowType)&w->w, attr);
    if (s == EGL_NO_SURFACE) {
        free(w);
        return s;
    }
    w->surface = s;
    pthread_mutex_lock(&s_win_lock);
    w->next = s_wins;
    s_wins = w;
    pthread_mutex_unlock(&s_win_lock);
    return s;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface s)
{
    EGLBoolean r = r_destroy_surface(dpy, s);
    pthread_mutex_lock(&s_win_lock);
    for (struct a11_win **p = &s_wins; *p; p = &(*p)->next)
        if ((*p)->surface == s) {
            struct a11_win *w = *p;
            *p = w->next;
            free(w);
            break;
        }
    pthread_mutex_unlock(&s_win_lock);
    return r;
}

EGLImageKHR eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attr)
{
    if (target == EGL_NATIVE_BUFFER_ANDROID) {
        struct a11_buf *w = wrap((ANativeWindowBuffer_t *)buffer);
        if (!w)
            return EGL_NO_IMAGE_KHR;
        buffer = (EGLClientBuffer)w;
    }
    return r_create_image(dpy, ctx, target, buffer, attr);
}

fn_t eglGetProcAddress(const char *name)
{
    if (!strcmp(name, "eglCreateWindowSurface")) return (fn_t)eglCreateWindowSurface;
    if (!strcmp(name, "eglDestroySurface")) return (fn_t)eglDestroySurface;
    if (!strcmp(name, "eglCreateImageKHR")) return (fn_t)eglCreateImageKHR;
    if (!strcmp(name, "eglGetProcAddress")) return (fn_t)eglGetProcAddress;
    fn_t f = NULL;
    if (!strncmp(name, "egl", 3))
        f = (fn_t)hyb_dlsym(s_egl, name);
    else if (s_gles2)
        f = (fn_t)hyb_dlsym(s_gles2, name);
    if (!f && r_getproc)
        f = r_getproc(name);
    return f;
}

__attribute__((constructor)) static void shim_init(void)
{
    char tr[PROPERTY_VALUE_MAX];
    property_get("debug.egl_shim.trace", tr, "0");
    s_trace = atoi(tr);
    hyb_set_root("/system/a11");
    s_egl = hyb_dlopen("libEGL_adreno.so", 0);
    s_gles2 = hyb_dlopen("libGLESv2_adreno.so", 0);
    if (!s_egl) {
        ALOGE("Android 11 libEGL_adreno: %s", hyb_dlerror());
        return;
    }
    r_getproc = hyb_dlsym(s_egl, "eglGetProcAddress");
    r_create_window = hyb_dlsym(s_egl, "eglCreateWindowSurface");
    r_destroy_surface = hyb_dlsym(s_egl, "eglDestroySurface");
    r_create_image = hyb_dlsym(s_egl, "eglCreateImageKHR");
    void *g = dlopen("/system/lib/hw/gralloc.taimen.so", RTLD_NOW);
    s_import = g ? dlsym(g, "taimen_gralloc_import") : NULL;
    ALOGI("Android 11 Adreno driver ready (gralloc import %s)", s_import ? "ok" : "MISSING");
}
