/* KitKat gralloc (module 0.2 + allocator + framebuffer) on top of the Android 11 QTI gralloc1
 * (gralloc.msm8998.so, loaded by libhyb). Buffers are therefore real ION buffers with the QTI
 * private_handle_t layout, the ones the Adreno driver expects.
 *
 * Buffers received from another process: the QTI gralloc1 imports the given handle keeping the
 * pointer, then closes and destroys it (delete) on the last release. So it gets a clone
 * (duplicated descriptors); the original stays with KitKat (GraphicBuffer closes it itself).
 * taimen_gralloc_import() hands this QTI handle to the EGL shim (Adreno driver). */
#define LOG_TAG "gralloc.taimen"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <time.h>
#include <cutils/native_handle.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include "gralloc1_a11.h"
#include <poll.h>
#define __user
#include "mdss/msm_mdp_ext.h"
#include "../hyb/hyb.h"

/* ------------------------------------------------------------------ gralloc1 QTI */
static gralloc1_device_t *g1;
static G1_PFN_CREATE_DESCRIPTOR g1_create;
static G1_PFN_DESTROY_DESCRIPTOR g1_destroy;
static G1_PFN_SET_DIMENSIONS g1_dims;
static G1_PFN_SET_FORMAT g1_format;
static G1_PFN_SET_USAGE g1_prod, g1_cons;
static G1_PFN_SET_LAYER_COUNT g1_layers;
static G1_PFN_ALLOCATE g1_alloc;
static G1_PFN_GET_STRIDE g1_stride;
static G1_PFN_RETAIN g1_retain, g1_release;
static G1_PFN_LOCK g1_lock;
static G1_PFN_LOCK_FLEX g1_lock_flex;
static G1_PFN_UNLOCK g1_unlock;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void g1_init(void)
{
    hyb_set_root("/system/a11");
    void *so = hyb_dlopen("/vendor/lib/hw/gralloc.msm8998.so", 0);
    struct hw_module_t *hmi = so ? hyb_dlsym(so, "HMI") : NULL;
    if (!hmi) {
        ALOGE("gralloc.msm8998 not found: %s", hyb_dlerror());
        return;
    }
    struct hw_device_t *dev = NULL;
    if (hmi->methods->open(hmi, GRALLOC_HARDWARE_MODULE_ID, &dev) || !dev) {
        ALOGE("cannot open the QTI gralloc1");
        return;
    }
    gralloc1_device_t *d = (gralloc1_device_t *)dev;
#define GET(var, type, id) var = (type)d->getFunction(d, id)
    GET(g1_create, G1_PFN_CREATE_DESCRIPTOR, G1_CREATE_DESCRIPTOR);
    GET(g1_destroy, G1_PFN_DESTROY_DESCRIPTOR, G1_DESTROY_DESCRIPTOR);
    GET(g1_dims, G1_PFN_SET_DIMENSIONS, G1_SET_DIMENSIONS);
    GET(g1_format, G1_PFN_SET_FORMAT, G1_SET_FORMAT);
    GET(g1_prod, G1_PFN_SET_USAGE, G1_SET_PRODUCER_USAGE);
    GET(g1_cons, G1_PFN_SET_USAGE, G1_SET_CONSUMER_USAGE);
    GET(g1_layers, G1_PFN_SET_LAYER_COUNT, G1_SET_LAYER_COUNT);
    GET(g1_alloc, G1_PFN_ALLOCATE, G1_ALLOCATE);
    GET(g1_stride, G1_PFN_GET_STRIDE, G1_GET_STRIDE);
    GET(g1_retain, G1_PFN_RETAIN, G1_RETAIN);
    GET(g1_release, G1_PFN_RETAIN, G1_RELEASE);
    GET(g1_lock, G1_PFN_LOCK, G1_LOCK);
    GET(g1_lock_flex, G1_PFN_LOCK_FLEX, G1_LOCK_FLEX);
    GET(g1_unlock, G1_PFN_UNLOCK, G1_UNLOCK);
#undef GET
    g1 = d;
    ALOGI("QTI gralloc1 ready");
}

static int ready(void)
{
    pthread_once(&s_once, g1_init);
    return g1 != NULL;
}

/* KitKat usage (32-bit) -> gralloc1 producer / consumer usages */
static void usage_split(int usage, uint64_t *prod, uint64_t *cons)
{
    uint64_t p = 0, c = 0;
    switch (usage & GRALLOC_USAGE_SW_READ_MASK) {
    case GRALLOC_USAGE_SW_READ_RARELY: p |= G1P_CPU_READ; c |= G1C_CPU_READ; break;
    case GRALLOC_USAGE_SW_READ_OFTEN: p |= G1P_CPU_READ | G1P_CPU_READ_OFTEN; c |= G1C_CPU_READ | G1C_CPU_READ_OFTEN; break;
    }
    switch (usage & GRALLOC_USAGE_SW_WRITE_MASK) {
    case GRALLOC_USAGE_SW_WRITE_RARELY: p |= G1P_CPU_WRITE; break;
    case GRALLOC_USAGE_SW_WRITE_OFTEN: p |= G1P_CPU_WRITE | G1P_CPU_WRITE_OFTEN; break;
    }
    if (usage & GRALLOC_USAGE_HW_TEXTURE) c |= G1C_GPU_TEXTURE;
    if (usage & GRALLOC_USAGE_HW_RENDER) p |= G1P_GPU_RENDER_TARGET;
    if (usage & GRALLOC_USAGE_HW_COMPOSER) c |= G1C_HWCOMPOSER;
    if (usage & GRALLOC_USAGE_HW_FB) c |= G1C_CLIENT_TARGET;
    if (usage & GRALLOC_USAGE_PROTECTED) p |= G1P_PROTECTED;
    if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) c |= G1C_VIDEO_ENCODER;
    if (usage & GRALLOC_USAGE_HW_CAMERA_WRITE) p |= G1P_CAMERA;
    if (usage & GRALLOC_USAGE_HW_CAMERA_READ) c |= G1C_CAMERA;
    if (usage & GRALLOC_USAGE_RENDERSCRIPT) c |= G1C_RENDERSCRIPT;
    /* QTI private bits (UBWC, video...): same positions in both APIs */
    p |= (uint64_t)(usage & 0xf0000000u);
    c |= (uint64_t)(usage & 0xf0000000u);
    *prod = p;
    *cons = c;
}

/* ------------------------------------------------------------------ imported handles */
struct entry {
    buffer_handle_t kk;     /* handle known to KitKat */
    buffer_handle_t qti;    /* handle given to gralloc1 (clone or the same) */
    int owned;              /* allocated here: released by free() */
    struct entry *next;
};
#define NBUCKET 256
static struct entry *s_map[NBUCKET];
static pthread_mutex_t s_map_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned slot(buffer_handle_t h) { return ((uintptr_t)h >> 3) % NBUCKET; }

static struct entry *find_locked(buffer_handle_t h)
{
    for (struct entry *e = s_map[slot(h)]; e; e = e->next)
        if (e->kk == h)
            return e;
    return NULL;
}

static void put_locked(buffer_handle_t kk, buffer_handle_t qti, int owned)
{
    struct entry *e = calloc(1, sizeof(*e));
    e->kk = kk;
    e->qti = qti;
    e->owned = owned;
    e->next = s_map[slot(kk)];
    s_map[slot(kk)] = e;
}

static struct entry *take_locked(buffer_handle_t h)
{
    for (struct entry **p = &s_map[slot(h)]; *p; p = &(*p)->next)
        if ((*p)->kk == h) {
            struct entry *e = *p;
            *p = e->next;
            return e;
        }
    return NULL;
}

static int valid(buffer_handle_t h)
{
    const struct qti_handle *q = (const struct qti_handle *)h;
    return h && h->numFds == 2 && h->numInts >= 20 && q->magic == QTI_HANDLE_MAGIC;
}

static native_handle_t *clone_handle(buffer_handle_t h)
{
    native_handle_t *c = native_handle_create(h->numFds, h->numInts);
    if (!c)
        return NULL;
    for (int i = 0; i < h->numFds; i++)
        c->data[i] = dup(h->data[i]);
    memcpy(&c->data[h->numFds], &h->data[h->numFds], h->numInts * sizeof(int));
    return c;
}

static int do_register(buffer_handle_t h)
{
    if (!ready() || !valid(h))
        return -EINVAL;
    /* base and base_metadata come through binder with addresses of the allocating process:
     * zero them, as the QTI import does, so that libqdMetaData (video codecs) maps the
     * metadata itself instead of writing to a foreign address */
    struct qti_handle *qh = (struct qti_handle *)h;
    qh->base = 0;
    qh->base_metadata = 0;
    pthread_mutex_lock(&s_map_lock);
    if (find_locked(h)) {
        pthread_mutex_unlock(&s_map_lock);
        return 0;
    }
    native_handle_t *c = clone_handle(h);
    int r = c ? g1_retain(g1, c) : -ENOMEM;
    if (r) {
        ALOGE("buffer import refused: %d", r);
        if (c) {
            native_handle_close(c);
            native_handle_delete(c);
        }
        pthread_mutex_unlock(&s_map_lock);
        return -EINVAL;
    }
    put_locked(h, c, 0);
    pthread_mutex_unlock(&s_map_lock);
    return 0;
}

/* matching QTI handle (for the Adreno driver); imported on the fly when needed */
buffer_handle_t taimen_gralloc_import(buffer_handle_t h)
{
    if (!h || !ready())
        return NULL;
    pthread_mutex_lock(&s_map_lock);
    struct entry *e = find_locked(h);
    buffer_handle_t q = e ? e->qti : NULL;
    pthread_mutex_unlock(&s_map_lock);
    if (q)
        return q;
    if (do_register(h))
        return NULL;
    pthread_mutex_lock(&s_map_lock);
    e = find_locked(h);
    q = e ? e->qti : NULL;
    pthread_mutex_unlock(&s_map_lock);
    return q;
}

/* ------------------------------------------------------------------ module */
static int gr_register(gralloc_module_t const *m, buffer_handle_t h) { return do_register(h); }

static int gr_unregister(gralloc_module_t const *m, buffer_handle_t h)
{
    pthread_mutex_lock(&s_map_lock);
    struct entry *e = take_locked(h);
    pthread_mutex_unlock(&s_map_lock);
    if (!e)
        return -EINVAL;
    if (!e->owned)
        g1_release(g1, e->qti);  /* closes and destroys the clone */
    free(e);
    return 0;
}

static int gr_lock(gralloc_module_t const *m, buffer_handle_t h, int usage, int l, int t, int w, int hh, void **vaddr)
{
    buffer_handle_t q = taimen_gralloc_import(h);
    if (!q)
        return -EINVAL;
    uint64_t p, c;
    usage_split(usage, &p, &c);
    gralloc1_rect_t r = { l, t, w, hh };
    int ret = g1_lock(g1, q, p, c, &r, vaddr, -1);
    return ret ? -EINVAL : 0;
}

static int gr_lock_ycbcr(gralloc_module_t const *m, buffer_handle_t h, int usage, int l, int t, int w, int hh,
                         struct android_ycbcr *out)
{
    buffer_handle_t q = taimen_gralloc_import(h);
    if (!q)
        return -EINVAL;
    uint64_t p, c;
    usage_split(usage, &p, &c);
    gralloc1_rect_t r = { l, t, w, hh };
    android_flex_plane_t planes[4];
    android_flex_layout_t flex = { 0, 4, planes };
    if (g1_lock_flex(g1, q, p, c, &r, &flex, -1))
        return -EINVAL;
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < flex.num_planes; i++) {
        android_flex_plane_t *pl = &flex.planes[i];
        if (pl->component == FLEX_COMPONENT_Y) {
            out->y = pl->top_left;
            out->ystride = pl->v_increment;
        } else if (pl->component == FLEX_COMPONENT_Cb) {
            out->cb = pl->top_left;
            out->cstride = pl->v_increment;
            out->chroma_step = pl->h_increment;
        } else if (pl->component == FLEX_COMPONENT_Cr) {
            out->cr = pl->top_left;
        }
    }
    return 0;
}

static int gr_unlock(gralloc_module_t const *m, buffer_handle_t h)
{
    buffer_handle_t q = taimen_gralloc_import(h);
    if (!q)
        return -EINVAL;
    int32_t fence = -1;
    int r = g1_unlock(g1, q, &fence);
    if (fence >= 0)
        close(fence);  /* already signalled for the CPU */
    return r ? -EINVAL : 0;
}

/* ------------------------------------------------------------------ allocator */
static int alloc_buffer(int w, int h, int format, int usage, buffer_handle_t *out, int *stride)
{
    if (!ready())
        return -ENODEV;
    uint64_t p, c;
    usage_split(usage, &p, &c);
    gralloc1_buffer_descriptor_t d;
    if (g1_create(g1, &d))
        return -ENOMEM;
    g1_dims(g1, d, w, h);
    g1_format(g1, d, format);
    g1_prod(g1, d, p);
    g1_cons(g1, d, c);
    if (g1_layers)
        g1_layers(g1, d, 1);
    buffer_handle_t b = NULL;
    int r = g1_alloc(g1, 1, &d, &b);
    g1_destroy(g1, d);
    if (r || !b) {
        ALOGE("allocation %dx%d format %#x usage %#x refused: %d", w, h, format, usage, r);
        return -ENOMEM;
    }
    uint32_t s = w;
    g1_stride(g1, b, &s);
    *stride = s;
    *out = b;
    pthread_mutex_lock(&s_map_lock);
    put_locked(b, b, 1);
    pthread_mutex_unlock(&s_map_lock);
    return 0;
}

static int gr_alloc(alloc_device_t *dev, int w, int h, int format, int usage, buffer_handle_t *out, int *stride)
{
    /* framebuffer buffers: readable by the CPU (copied to fb0 on post) */
    if (usage & GRALLOC_USAGE_HW_FB)
        usage |= GRALLOC_USAGE_SW_READ_OFTEN;
    return alloc_buffer(w, h, format, usage, out, stride);
}

static int gr_free(alloc_device_t *dev, buffer_handle_t h)
{
    pthread_mutex_lock(&s_map_lock);
    struct entry *e = take_locked(h);
    pthread_mutex_unlock(&s_map_lock);
    if (!e)
        return -EINVAL;
    g1_release(g1, e->qti);  /* buffer allocated here: released and handle destroyed */
    free(e);
    return 0;
}

static int dev_close(struct hw_device_t *d)
{
    free(d);
    return 0;
}

/* ------------------------------------------------------------------ framebuffer (fb0) */
/* command mode DSI panel: each frame is copied to the hidden page of fb0, then a pan sends it
 * to the panel (fallback when fbpresentd is not used) */
struct fb_ctx {
    framebuffer_device_t dev;
    int fd;
    uint8_t *base;
    uint32_t stride;
    int back, flips, swap_rb;
    struct fb_var_screeninfo var;
    pthread_mutex_t lock;
    int commit_ok;      /* presentation by fbpresentd (otherwise copy + pan) */
    int sock;           /* connection to fbpresentd */
    uint64_t sent[8];   /* ids of the buffers whose descriptor was already sent */
    int next_sent;
};

/* zero-copy presentation. The SurfaceFlinger buffer (ION) becomes the only layer of the DMA0
 * pipe of the main display, through an MDSS atomic commit like the stock composer does.
 * The commit is done by fbpresentd (64-bit): from a 32-bit process, the kernel compat path of
 * MSMFB_ATOMIC_COMMIT crashes the system. The descriptor of each buffer is sent once
 * (SCM_RIGHTS), then only its id. */
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include "../fbpresentd/fbpresent.h"

static int fbp_connect(void)
{
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path + 1, FBP_SOCKET);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(FBP_SOCKET);
    for (int i = 0; i < 20; i++) {
        int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (s >= 0 && connect(s, (struct sockaddr *)&a, len) == 0)
            return s;
        if (s >= 0)
            close(s);
        usleep(100000);
    }
    return -1;
}

static int fb_commit(struct fb_ctx *f, const struct qti_handle *qh)
{
    int known = 0;
    for (int i = 0; i < 8; i++)
        known |= f->sent[i] == qh->id;
    struct fbp_req r = { qh->id, (uint32_t)qh->width, (uint32_t)qh->height, qh->offset,
                         (qh->flags & 0x08000000) ? 1u : 0u, f->var.xres, f->var.yres };
    struct iovec iov = { &r, sizeof(r) };
    struct msghdr m;
    char ctl[CMSG_SPACE(sizeof(int))];
    memset(&m, 0, sizeof(m));
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (!known) {
        m.msg_control = ctl;
        m.msg_controllen = sizeof(ctl);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &qh->fd, sizeof(int));
    }
    if (sendmsg(f->sock, &m, MSG_NOSIGNAL) != sizeof(r))
        return -errno;
    int32_t ret;
    if (recv(f->sock, &ret, sizeof(ret), 0) != sizeof(ret))
        return -EPIPE;
    if (ret == 0 && !known) {
        f->sent[f->next_sent] = qh->id;
        f->next_sent = (f->next_sent + 1) % 8;
    }
    return ret;
}

static inline uint32_t swap_rb(uint32_t px)
{
    return (px & 0xff00ff00) | ((px & 0xff) << 16) | ((px >> 16) & 0xff);
}

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static int fb_post(struct framebuffer_device_t *dev, buffer_handle_t buffer)
{
    struct fb_ctx *f = (struct fb_ctx *)dev;
    /* statistics (debug.gralloc.fbstats=1): cost of the copy and the pan, every 120 frames */
    static int frames;
    static int64_t t_copy, t_pan;
    int64_t t0 = now_us();
    const struct qti_handle *qh = (const struct qti_handle *)taimen_gralloc_import(buffer);
    if (!qh)
        return -EINVAL;
    if (f->commit_ok) {
        pthread_mutex_lock(&f->lock);
        int r = fb_commit(f, qh);
        pthread_mutex_unlock(&f->lock);
        if (r == 0) {
            int64_t t1 = now_us();
            t_copy += t1 - t0;
            if (++frames == 120) {
                char v[PROPERTY_VALUE_MAX];
                property_get("debug.gralloc.fbstats", v, "0");
                if (atoi(v))
                    ALOGI("post: commit %.2f ms per frame", t_copy / 120000.0);
                frames = 0;
                t_copy = 0;
            }
            return 0;
        }
        ALOGE("presentation by fbpresentd refused (%s): falling back to copy + pan", strerror(-r));
        f->commit_ok = 0;
    }
    void *src = NULL;
    gralloc1_rect_t r = { 0, 0, (int)f->var.xres, (int)f->var.yres };
    if (g1_lock(g1, (buffer_handle_t)qh, G1P_CPU_READ | G1P_CPU_READ_OFTEN, G1C_CPU_READ | G1C_CPU_READ_OFTEN,
                &r, &src, -1)) {
        ALOGE("post: cannot lock");
        return -EINVAL;
    }
    pthread_mutex_lock(&f->lock);
    uint32_t sstride = qh->width * 4;
    uint8_t *dst = f->base + f->back * f->var.yres * f->stride;
    for (uint32_t y = 0; y < f->var.yres; y++) {
        const uint32_t *s = (const uint32_t *)((uint8_t *)src + y * sstride);
        uint32_t *d = (uint32_t *)(dst + y * f->stride);
        if (f->swap_rb) {
            for (uint32_t x = 0; x < f->var.xres; x++)
                d[x] = swap_rb(s[x]);
        } else {
            memcpy(d, s, f->var.xres * 4);
        }
    }
    int32_t fence = -1;
    g1_unlock(g1, (buffer_handle_t)qh, &fence);
    if (fence >= 0)
        close(fence);
    int64_t t1 = now_us();
    f->var.yoffset = f->flips ? f->back * f->var.yres : 0;
    f->var.activate = FB_ACTIVATE_VBL;
    if (ioctl(f->fd, FBIOPAN_DISPLAY, &f->var) < 0)
        ALOGE("pan: %s", strerror(errno));
    int64_t t2 = now_us();
    t_copy += t1 - t0;
    t_pan += t2 - t1;
    if (++frames == 120) {
        char v[PROPERTY_VALUE_MAX];
        property_get("debug.gralloc.fbstats", v, "0");
        if (atoi(v))
            ALOGI("post: copy %.2f ms, pan %.2f ms per frame", t_copy / 120000.0, t_pan / 120000.0);
        frames = 0;
        t_copy = t_pan = 0;
    }
    if (f->flips)
        f->back ^= 1;
    pthread_mutex_unlock(&f->lock);
    return 0;
}

static int fb_swap_interval(struct framebuffer_device_t *dev, int interval) { return 0; }

static int fb_enable_screen(struct framebuffer_device_t *dev, int enable)
{
    struct fb_ctx *f = (struct fb_ctx *)dev;
    return ioctl(f->fd, FBIOBLANK, enable ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN) < 0 ? -errno : 0;
}

static int fb_close(struct hw_device_t *d)
{
    struct fb_ctx *f = (struct fb_ctx *)d;
    close(f->fd);
    free(f);
    return 0;
}

static int fb_open(const struct hw_module_t *module, struct hw_device_t **out)
{
    if (!ready())
        return -ENODEV;
    struct fb_ctx *f = calloc(1, sizeof(*f));
    f->fd = open("/dev/graphics/fb0", O_RDWR);
    if (f->fd < 0) {
        free(f);
        return -errno;
    }
    struct fb_fix_screeninfo fix;
    ioctl(f->fd, FBIOBLANK, FB_BLANK_UNBLANK);
    ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var);
    if (f->var.yres_virtual < f->var.yres * 2) {
        f->var.yres_virtual = f->var.yres * 2;
        f->var.activate = FB_ACTIVATE_NOW;
        ioctl(f->fd, FBIOPUT_VSCREENINFO, &f->var);
        ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var);
    }
    ioctl(f->fd, FBIOGET_FSCREENINFO, &fix);
    f->stride = fix.line_length;
    f->swap_rb = f->var.red.offset == 16;
    f->flips = f->var.yres_virtual >= f->var.yres * 2;
    f->back = f->flips ? 1 : 0;
    f->base = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->base == MAP_FAILED) {
        close(f->fd);
        free(f);
        return -ENOMEM;
    }
    pthread_mutex_init(&f->lock, NULL);
    char cm[PROPERTY_VALUE_MAX];
    property_get("persist.gralloc.fb_commit", cm, "1");
    f->sock = atoi(cm) ? fbp_connect() : -1;
    f->commit_ok = f->sock >= 0;
    framebuffer_device_t *d = &f->dev;
    d->common.tag = HARDWARE_DEVICE_TAG;
    d->common.version = 0;
    d->common.module = (struct hw_module_t *)module;
    d->common.close = fb_close;
    /* const fields of framebuffer_device_t: filled once here */
    *(uint32_t *)&d->flags = 0;
    *(uint32_t *)&d->width = f->var.xres;
    *(uint32_t *)&d->height = f->var.yres;
    *(int *)&d->stride = f->var.xres;
    *(int *)&d->format = HAL_PIXEL_FORMAT_RGBA_8888;
    float wmm = f->var.width > 0 ? f->var.width : 68, hmm = f->var.height > 0 ? f->var.height : 136;
    *(float *)&d->xdpi = f->var.xres * 25.4f / wmm;
    *(float *)&d->ydpi = f->var.yres * 25.4f / hmm;
    *(float *)&d->fps = 60.0f;
    *(int *)&d->minSwapInterval = 1;
    *(int *)&d->maxSwapInterval = 1;
    *(int *)&d->numFramebuffers = 3;
    d->setSwapInterval = fb_swap_interval;
    d->post = fb_post;
    d->enableScreen = fb_enable_screen;
    ALOGI("fb0 %ux%u, line %u bytes, double buffer %d, BGRA %d, atomic commit %d", f->var.xres,
          f->var.yres, f->stride, f->flips, f->swap_rb, f->commit_ok);
    *out = &d->common;
    return 0;
}

static int gr_device_open(const struct hw_module_t *module, const char *name, struct hw_device_t **out)
{
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        if (!ready())
            return -ENODEV;
        alloc_device_t *d = calloc(1, sizeof(*d));
        d->common.tag = HARDWARE_DEVICE_TAG;
        d->common.version = 0;
        d->common.module = (struct hw_module_t *)module;
        d->common.close = dev_close;
        d->alloc = gr_alloc;
        d->free = gr_free;
        *out = &d->common;
        return 0;
    }
    if (!strcmp(name, GRALLOC_HARDWARE_FB0))
        return fb_open(module, out);
    return -EINVAL;
}

static struct hw_module_methods_t gr_methods = { .open = gr_device_open };

struct gralloc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = GRALLOC_MODULE_API_VERSION_0_2,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = GRALLOC_HARDWARE_MODULE_ID,
        .name = "Pixel 2 XL: Android 11 QTI gralloc1 through libhyb",
        .author = "taimen-kk",
        .methods = &gr_methods,
    },
    .registerBuffer = gr_register,
    .unregisterBuffer = gr_unregister,
    .lock = gr_lock,
    .unlock = gr_unlock,
    .lock_ycbcr = gr_lock_ycbcr,
};
