/* KitKat camera HAL on top of the Android 11 Qualcomm HAL (camera.msm8998.so, HAL3 device 3.5),
 * loaded by libhyb into mediaserver.
 *
 * KitKat expects a module 2.0 and HAL3 devices of version 3.0 exactly. This module translates:
 *  - metadata: the binary formats differ (Android 11 has a longer header), so it is copied entry
 *    by entry. Tag numbers differ too: Android 11 removed the geometric and hotPixel.info
 *    sections and the following ones moved. Tags are matched by full name (section.tag);
 *    tags unknown to KitKat and vendor tags are dropped; the old static keys KitKat reads (JPEG
 *    sizes and durations, processed/raw durations) are rebuilt from
 *    availableStreamConfigurations / availableMinFrameDurations;
 *  - streams: Android 11's camera3_stream_t is longer (data_space, rotation, physical camera):
 *    each KitKat stream has a twin, which the HAL keeps from one configuration to the next;
 *  - results: Android 11 sends them in several parts, KitKat wants a single metadata buffer per
 *    frame and in frame order: the parts are merged, then delivered in order. KitKat also
 *    rejects any result (buffers included) before the shutter notification of the frame,
 *    which Android 11 allows: buffers and metadata wait for the shutter.
 * The buffers themselves already are QTI private_handle_t (gralloc.taimen): nothing to convert. */
#define LOG_TAG "camera.taimen"
#include <errno.h>
#include <stddef.h>
#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cutils/properties.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/camera_metadata.h>
#include <cutils/log.h>
#include "hyb.h"

/* ------------------------------------------------------------------ Android 11 camera HAL */
struct a11_camera_info {
    int facing;
    int orientation;
    uint32_t device_version;
    const camera_metadata_t *static_camera_characteristics;  /* Android 11 format */
    int resource_cost;
    char **conflicting_devices;
    size_t conflicting_devices_length;
};
struct a11_module_callbacks {
    void (*camera_device_status_change)(const struct a11_module_callbacks *, int, int);
    void (*torch_mode_status_change)(const struct a11_module_callbacks *, const char *, int);
};
struct a11_camera_module {
    struct hw_module_t common;
    int (*get_number_of_cameras)(void);
    int (*get_camera_info)(int id, struct a11_camera_info *info);
    int (*set_callbacks)(const struct a11_module_callbacks *callbacks);
    void (*get_vendor_tag_ops)(void *ops);
    int (*open_legacy)(const struct hw_module_t *, const char *, uint32_t, struct hw_device_t **);
    int (*set_torch_mode)(const char *id, int enabled);
    int (*init)(void);
    void *reserved[5];
};
struct a11_stream {
    int stream_type;
    uint32_t width;
    uint32_t height;
    int format;
    uint32_t usage;
    uint32_t max_buffers;
    void *priv;
    int32_t data_space;
    int rotation;
    const char *physical_camera_id;
    void *reserved[6];
};
struct a11_stream_buffer {
    struct a11_stream *stream;
    buffer_handle_t *buffer;
    int status;
    int acquire_fence;
    int release_fence;
};
struct a11_stream_configuration {
    uint32_t num_streams;
    struct a11_stream **streams;
    uint32_t operation_mode;
    const camera_metadata_t *session_parameters;
};
struct a11_capture_request {
    uint32_t frame_number;
    const camera_metadata_t *settings;
    struct a11_stream_buffer *input_buffer;
    uint32_t num_output_buffers;
    const struct a11_stream_buffer *output_buffers;
    uint32_t num_physcam_settings;
    const char **physcam_id;
    const camera_metadata_t **physcam_settings;
};
struct a11_capture_result {
    uint32_t frame_number;
    const camera_metadata_t *result;
    uint32_t num_output_buffers;
    const struct a11_stream_buffer *output_buffers;
    const struct a11_stream_buffer *input_buffer;
    uint32_t partial_result;
    uint32_t num_physcam_metadata;
    const char **physcam_ids;
    const camera_metadata_t **physcam_metadata;
};
struct a11_notify_msg {
    int type;
    union {
        struct { uint32_t frame_number; struct a11_stream *error_stream; int error_code; } error;
        struct { uint32_t frame_number; uint64_t timestamp; } shutter;
        uint8_t generic[32];
    } message;
};
struct a11_callback_ops {
    void (*process_capture_result)(const struct a11_callback_ops *, const struct a11_capture_result *);
    void (*notify)(const struct a11_callback_ops *, const struct a11_notify_msg *);
    void *request_stream_buffers;  /* HAL buffer management (3.6): unused in 3.5 */
    void *return_stream_buffers;
};
struct a11_device;
struct a11_device_ops {
    int (*initialize)(const struct a11_device *, const struct a11_callback_ops *);
    int (*configure_streams)(const struct a11_device *, struct a11_stream_configuration *);
    void *register_stream_buffers;
    const camera_metadata_t *(*construct_default_request_settings)(const struct a11_device *, int type);
    int (*process_capture_request)(const struct a11_device *, struct a11_capture_request *);
    void (*get_metadata_vendor_tag_ops)(const struct a11_device *, void *);
    void (*dump)(const struct a11_device *, int fd);
    int (*flush)(const struct a11_device *);
    void *reserved[8];
};
struct a11_device {
    hw_device_t common;
    struct a11_device_ops *ops;
    void *priv;
};

/* Android 11 libcamera_metadata (the KitKat one is linked normally) */
static struct {
    camera_metadata_t *(*allocate)(size_t entries, size_t data);
    void (*free)(camera_metadata_t *m);
    size_t (*entry_count)(const camera_metadata_t *m);
    size_t (*data_count)(const camera_metadata_t *m);
    int (*get_entry)(const camera_metadata_t *m, size_t index, camera_metadata_entry_t *e);
    int (*add_entry)(camera_metadata_t *m, uint32_t tag, const void *data, size_t count);
    int (*append)(camera_metadata_t *dst, const camera_metadata_t *src);
    int (*find_ro)(const camera_metadata_t *m, uint32_t tag, camera_metadata_ro_entry_t *e);
    camera_metadata_t *(*clone)(const camera_metadata_t *src);
    const char *(*tag_name)(uint32_t tag);
    int (*set_vendor_ops)(const void *ops);
    unsigned int (*section_bounds)[2];
    const char **section_names;
} A;
#define A11_SECTION_COUNT 30  /* size of Android 11's camera_metadata_section_names (120 bytes) */

/* Android 11 tags (read from the HAL metadata) */
#define TAG_PARTIAL_RESULT_COUNT 0xC000B
#define TAG_SCALER_AVAILABLE_FORMATS 0xD0001
#define TAG_SCALER_JPEG_MIN_DURATIONS 0xD0002
#define TAG_SCALER_JPEG_SIZES 0xD0003
#define TAG_SCALER_PROCESSED_MIN_DURATIONS 0xD0005
#define TAG_SCALER_PROCESSED_SIZES 0xD0006
#define TAG_SCALER_RAW_MIN_DURATIONS 0xD0007
#define TAG_SCALER_RAW_SIZES 0xD0008
#define TAG_SCALER_STREAM_CONFIGURATIONS 0xD000A
#define TAG_SCALER_MIN_FRAME_DURATIONS 0xD000B
#define FMT_BLOB 0x21
#define FMT_IMPL_DEFINED 0x22
#define FMT_RAW16 0x20
#define DATASPACE_V0_JFIF 0x08C20000

#define MAX_CAMERAS 4
#define MAX_STREAMS 16
#define MAX_PENDING 64

static struct a11_camera_module *s_hal;
static int s_count;
static camera_metadata_t *s_static[MAX_CAMERAS];   /* KitKat format */
static struct a11_camera_info s_a11_info[MAX_CAMERAS];
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

/* ------------------------------------------------------------------ tags */
#define NO_TAG 0xffffffffu
static uint32_t *s_a2k[A11_SECTION_COUNT];      /* Android 11 -> KitKat, per section */
static uint32_t *s_k2a[ANDROID_SECTION_COUNT];  /* KitKat -> Android 11 */

static void build_tag_maps(void)
{
    char full[160];
    for (int as = 0; as < A11_SECTION_COUNT; as++) {
        unsigned lo = A.section_bounds[as][0], hi = A.section_bounds[as][1];
        s_a2k[as] = malloc((hi - lo) * sizeof(uint32_t));
        for (unsigned t = lo; t < hi; t++)
            s_a2k[as][t - lo] = NO_TAG;
    }
    int mapped = 0;
    for (int ks = 0; ks < ANDROID_SECTION_COUNT; ks++) {
        unsigned lo = camera_metadata_section_bounds[ks][0], hi = camera_metadata_section_bounds[ks][1];
        s_k2a[ks] = malloc((hi - lo) * sizeof(uint32_t));
        for (unsigned kt = lo; kt < hi; kt++) {
            s_k2a[ks][kt - lo] = NO_TAG;
            snprintf(full, sizeof(full), "%s.%s", camera_metadata_section_names[ks], get_camera_metadata_tag_name(kt));
            for (int as = 0; as < A11_SECTION_COUNT && s_k2a[ks][kt - lo] == NO_TAG; as++) {
                size_t sl = strlen(A.section_names[as]);
                if (strncmp(full, A.section_names[as], sl) || full[sl] != '.')
                    continue;
                for (unsigned at = A.section_bounds[as][0]; at < A.section_bounds[as][1]; at++)
                    if (!strcmp(full + sl + 1, A.tag_name(at))) {
                        s_k2a[ks][kt - lo] = at;
                        s_a2k[as][at - A.section_bounds[as][0]] = kt;
                        mapped++;
                        break;
                    }
            }
        }
    }
    ALOGI("%d tags common to KitKat and Android 11", mapped);
}

static uint32_t a11_to_kk_tag(uint32_t t)
{
    unsigned sec = t >> 16;
    if (sec >= A11_SECTION_COUNT || t >= A.section_bounds[sec][1])
        return NO_TAG;
    return s_a2k[sec][t - A.section_bounds[sec][0]];
}

static uint32_t kk_to_a11_tag(uint32_t t)
{
    unsigned sec = t >> 16;
    if (sec >= ANDROID_SECTION_COUNT || t >= camera_metadata_section_bounds[sec][1])
        return NO_TAG;
    return s_k2a[sec][t - camera_metadata_section_bounds[sec][0]];
}

/* ------------------------------------------------------------------ metadata */
/* Android 11 -> KitKat. extra: rebuilt entries (static metadata) appended after the copy. */
struct extra {
    uint32_t tag;
    const void *data;
    size_t count;
};

static camera_metadata_t *to_kk(const camera_metadata_t *src, const struct extra *ex, int nex)
{
    size_t n = A.entry_count(src), entries = 0, data = 0;
    camera_metadata_entry_t e;
    for (size_t i = 0; i < n; i++) {
        uint32_t kt;
        if (A.get_entry(src, i, &e) || (kt = a11_to_kk_tag(e.tag)) == NO_TAG || get_camera_metadata_tag_type(kt) != e.type)
            continue;
        entries++;
        data += calculate_camera_metadata_entry_data_size(e.type, e.count);
    }
    for (int i = 0; i < nex; i++) {
        entries++;
        data += calculate_camera_metadata_entry_data_size(get_camera_metadata_tag_type(ex[i].tag), ex[i].count);
    }
    camera_metadata_t *dst = allocate_camera_metadata(entries + 1, data + 16);
    if (!dst)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t kt;
        if (A.get_entry(src, i, &e) || (kt = a11_to_kk_tag(e.tag)) == NO_TAG || get_camera_metadata_tag_type(kt) != e.type)
            continue;
        add_camera_metadata_entry(dst, kt, e.data.u8, e.count);
    }
    for (int i = 0; i < nex; i++)
        add_camera_metadata_entry(dst, ex[i].tag, ex[i].data, ex[i].count);
    return dst;
}

/* KitKat -> Android 11. base: HAL template (Android 11 format) whose entries KitKat does not
 * provide are kept (vendor tags, recent keys): the Android 11 framework always sends complete
 * settings, and without them the preview 3A does not start. */
static camera_metadata_t *to_a11(const camera_metadata_t *src, const camera_metadata_t *base)
{
    size_t n = get_camera_metadata_entry_count(src), nb = base ? A.entry_count(base) : 0;
    camera_metadata_t *dst = A.allocate(n + nb + 1, get_camera_metadata_data_count(src) +
                                                        (base ? A.data_count(base) : 0) + 16);
    if (!dst)
        return NULL;
    camera_metadata_entry_t e;
    camera_metadata_ro_entry_t ro;
    for (size_t i = 0; i < n; i++) {
        uint32_t at;
        if (get_camera_metadata_entry((camera_metadata_t *)src, i, &e) == 0 && (at = kk_to_a11_tag(e.tag)) != NO_TAG)
            A.add_entry(dst, at, e.data.u8, e.count);
    }
    for (size_t i = 0; i < nb; i++)
        if (A.get_entry(base, i, &e) == 0 && A.find_ro(dst, e.tag, &ro) != 0)
            A.add_entry(dst, e.tag, e.data.u8, e.count);
    return dst;
}

/* old KitKat static keys, rebuilt from the stream configurations */
static camera_metadata_t *convert_static(const camera_metadata_t *s)
{
    /* dropped tags, for the log (debug.camera.taimen.probe=1) */
    char dbg[PROPERTY_VALUE_MAX];
    property_get("debug.camera.taimen.probe", dbg, "0");
    size_t n = atoi(dbg) ? A.entry_count(s) : 0;
    camera_metadata_entry_t de;
    for (size_t i = 0; i < n; i++)
        if (A.get_entry(s, i, &de) == 0 && !(de.tag & 0x80000000) &&
            (a11_to_kk_tag(de.tag) == NO_TAG || get_camera_metadata_tag_type(a11_to_kk_tag(de.tag)) != de.type))
            ALOGD("static dropped: %#x %s (type %d)", de.tag, A.tag_name(de.tag), de.type);
    camera_metadata_ro_entry_t cfg, dur, e;
    struct extra ex[4];
    int nex = 0;
    static int32_t sizes[MAX_CAMERAS][2][256];
    static int64_t durs[MAX_CAMERAS][3][128];
    static int slot;
    int k = slot++ % MAX_CAMERAS;
    if (A.find_ro(s, TAG_SCALER_STREAM_CONFIGURATIONS, &cfg) || A.find_ro(s, TAG_SCALER_MIN_FRAME_DURATIONS, &dur)) {
        ALOGW("no stream configurations in the static metadata");
        return to_kk(s, NULL, 0);
    }
    /* minimum duration of a (format, size) configuration */
#define MIN_DURATION(fmt, w, h) ({                                                        \
        int64_t _d = 33333333;                                                            \
        for (size_t _j = 0; _j + 3 < dur.count; _j += 4)                                  \
            if (dur.data.i64[_j] == (fmt) && dur.data.i64[_j + 1] == (w) && dur.data.i64[_j + 2] == (h)) \
                _d = dur.data.i64[_j + 3];                                                \
        _d; })
    if (A.find_ro(s, TAG_SCALER_JPEG_SIZES, &e)) {
        int n = 0, nd = 0;
        for (size_t i = 0; i + 3 < cfg.count && n + 2 <= 256 && nd < 128; i += 4)
            if (cfg.data.i32[i] == FMT_BLOB && cfg.data.i32[i + 3] == 0) {
                sizes[k][0][n++] = cfg.data.i32[i + 1];
                sizes[k][0][n++] = cfg.data.i32[i + 2];
                durs[k][0][nd++] = MIN_DURATION(FMT_BLOB, cfg.data.i32[i + 1], cfg.data.i32[i + 2]);
            }
        ex[nex++] = (struct extra){ ANDROID_SCALER_AVAILABLE_JPEG_SIZES, sizes[k][0], (size_t)n };
        ex[nex++] = (struct extra){ ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS, durs[k][0], (size_t)nd };
    }
    if (A.find_ro(s, TAG_SCALER_PROCESSED_MIN_DURATIONS, &e) && A.find_ro(s, TAG_SCALER_PROCESSED_SIZES, &e) == 0) {
        int nd = 0;
        for (size_t i = 0; i + 1 < e.count && nd < 128; i += 2)
            durs[k][1][nd++] = MIN_DURATION(FMT_IMPL_DEFINED, e.data.i32[i], e.data.i32[i + 1]);
        ex[nex++] = (struct extra){ ANDROID_SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS, durs[k][1], (size_t)nd };
    }
    if (A.find_ro(s, TAG_SCALER_RAW_MIN_DURATIONS, &e) && A.find_ro(s, TAG_SCALER_RAW_SIZES, &e) == 0) {
        int nd = 0;
        for (size_t i = 0; i + 1 < e.count && nd < 128; i += 2)
            durs[k][2][nd++] = MIN_DURATION(FMT_RAW16, e.data.i32[i], e.data.i32[i + 1]);
        ex[nex++] = (struct extra){ ANDROID_SCALER_AVAILABLE_RAW_MIN_DURATIONS, durs[k][2], (size_t)nd };
    }
#undef MIN_DURATION
    camera_metadata_t *kk = to_kk(s, ex, nex);
    /* android.control.maxRegions: Android 11 gives 3 (AE, AWB, AF), KitKat a single one for
     * metering and focus areas */
    camera_metadata_entry_t mr;
    if (kk && find_camera_metadata_entry(kk, ANDROID_CONTROL_MAX_REGIONS, &mr) == 0 && mr.count == 3) {
        int32_t one = mr.data.i32[0] > mr.data.i32[2] ? mr.data.i32[0] : mr.data.i32[2];
        update_camera_metadata_entry(kk, mr.index, &one, 1, NULL);
    }
    return kk;
}

/* ------------------------------------------------------------------ module */
static void dev_status(const struct a11_module_callbacks *cb, int id, int st) { ALOGI("camera %d: status %d", id, st); }
static void torch_status(const struct a11_module_callbacks *cb, const char *id, int st) {}
static const struct a11_module_callbacks s_module_cbs = { dev_status, torch_status };

static void load_hal(void)
{
    hyb_set_root("/system/a11");
    void *md = hyb_dlopen("libcamera_metadata.so", 0);
    void *so = hyb_dlopen("/vendor/lib/hw/camera.msm8998.so", 0);
    if (!md || !so) {
        ALOGE("cannot load the Android 11 camera HAL: %s", hyb_dlerror());
        return;
    }
    A.allocate = hyb_dlsym(md, "allocate_camera_metadata");
    A.free = hyb_dlsym(md, "free_camera_metadata");
    A.entry_count = hyb_dlsym(md, "get_camera_metadata_entry_count");
    A.data_count = hyb_dlsym(md, "get_camera_metadata_data_count");
    A.get_entry = hyb_dlsym(md, "get_camera_metadata_entry");
    A.add_entry = hyb_dlsym(md, "add_camera_metadata_entry");
    A.append = hyb_dlsym(md, "append_camera_metadata");
    A.find_ro = hyb_dlsym(md, "find_camera_metadata_ro_entry");
    A.clone = hyb_dlsym(md, "clone_camera_metadata");
    A.tag_name = hyb_dlsym(md, "get_camera_metadata_tag_name");
    A.set_vendor_ops = hyb_dlsym(md, "set_camera_metadata_vendor_ops");
    A.section_bounds = hyb_dlsym(md, "camera_metadata_section_bounds");
    A.section_names = hyb_dlsym(md, "camera_metadata_section_names");
    struct a11_camera_module *m = hyb_dlsym(so, "HMI");
    if (!m || !A.allocate || !A.get_entry || !A.add_entry || !A.find_ro || !A.clone || !A.append || !A.tag_name ||
        !A.section_bounds || !A.section_names) {
        ALOGE("camera HAL symbols not found");
        return;
    }
    build_tag_maps();
    /* vendor tags of the HAL, registered as the Android 11 camera provider does: without
     * them, the HAL cannot write its own entries ("Unknown tag") */
    static struct { void *fn[5]; void *reserved[8]; } vendor_ops;
    if (m->get_vendor_tag_ops && A.set_vendor_ops) {
        m->get_vendor_tag_ops(&vendor_ops);
        if (vendor_ops.fn[0])
            A.set_vendor_ops(&vendor_ops);
    }
    if (m->init && m->init())
        ALOGW("camera HAL init failed");
    int n = m->get_number_of_cameras();  /* creates the QCamera2 factory: before set_callbacks */
    if (m->set_callbacks)
        m->set_callbacks(&s_module_cbs);
    if (n > MAX_CAMERAS)
        n = MAX_CAMERAS;
    for (int i = 0; i < n; i++) {
        if (m->get_camera_info(i, &s_a11_info[i]) || !s_a11_info[i].static_camera_characteristics) {
            ALOGE("camera %d: no information", i);
            n = i;
            break;
        }
        s_static[i] = convert_static(s_a11_info[i].static_camera_characteristics);
        ALOGI("camera %d: %s, orientation %d, %zu static entries (%zu on the Android 11 side)", i,
              s_a11_info[i].facing ? "front" : "back", s_a11_info[i].orientation,
              s_static[i] ? get_camera_metadata_entry_count(s_static[i]) : 0,
              A.entry_count(s_a11_info[i].static_camera_characteristics));
    }
    s_hal = m;
    s_count = n;
}

static int get_number_of_cameras(void)
{
    pthread_once(&s_once, load_hal);
    return s_count;
}

static int get_camera_info(int id, struct camera_info *info)
{
    pthread_once(&s_once, load_hal);
    if (id < 0 || id >= s_count)
        return -EINVAL;
    info->facing = s_a11_info[id].facing;
    info->orientation = s_a11_info[id].orientation;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_0;
    info->static_camera_characteristics = s_static[id];
    return 0;
}

/* ------------------------------------------------------------------ device */
struct pending {
    uint32_t frame;
    int used, skip;            /* skip: result error, no metadata to deliver */
    int errored;               /* request error: KitKat then accepts returns without a shutter */
    camera_metadata_t *parts;  /* parts received, Android 11 format */
    camera_metadata_t *ready;  /* complete metadata, KitKat format, waiting for its turn */
    camera3_stream_buffer_t bufs[MAX_STREAMS];  /* buffers that arrived before the shutter */
    int nbufs;
};

struct tdev {
    camera3_device_t kk;            /* first: KitKat only knows this part */
    camera3_device_ops_t kk_ops;
    struct a11_callback_ops a11_cb;
    struct a11_device *a;
    const camera3_callback_ops_t *kk_cb;
    int id;
    uint32_t partial_count;
    pthread_mutex_t lock;          /* streams, pending results */
    struct { camera3_stream_t *kk; struct a11_stream *a; } streams[MAX_STREAMS];
    struct pending pend[MAX_PENDING];
    uint32_t next_meta;             /* next frame whose metadata KitKat expects */
    int next_meta_set;
    uint32_t last_shutter;          /* shutters arrive in frame order */
    int have_shutter;
    camera_metadata_t *templates[8]; /* KitKat format, valid until close */
};

#define TDEV(d) ((struct tdev *)(d))
#define FROM_CB(cb) ((struct tdev *)((char *)(cb) - offsetof(struct tdev, a11_cb)))

static struct a11_stream *a11_stream_of(struct tdev *t, camera3_stream_t *s)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (t->streams[i].kk == s)
            return t->streams[i].a;
    return NULL;
}

static camera3_stream_t *kk_stream_of(struct tdev *t, struct a11_stream *s)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (t->streams[i].a == s)
            return t->streams[i].kk;
    return NULL;
}

static struct pending *pending_of(struct tdev *t, uint32_t frame, int create)
{
    for (int i = 0; i < MAX_PENDING; i++)
        if (t->pend[i].used && t->pend[i].frame == frame)
            return &t->pend[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_PENDING; i++)
        if (!t->pend[i].used) {
            memset(&t->pend[i], 0, sizeof(t->pend[i]));
            t->pend[i].used = 1;
            t->pend[i].frame = frame;
            return &t->pend[i];
        }
    ALOGE("too many pending results (frame %u)", frame);
    return NULL;
}

static int shuttered(struct tdev *t, struct pending *p, uint32_t frame)
{
    return (t->have_shutter && (int32_t)(frame - t->last_shutter) <= 0) || (p && p->errored);
}

/* diagnostics (debug.camera.taimen.probe=1): does the returned buffer hold data? */
static void probe_buffer(uint32_t frame, const camera3_stream_buffer_t *b)
{
    static int checked, enabled = -1;
    if (enabled < 0) {
        char v[PROPERTY_VALUE_MAX];
        property_get("debug.camera.taimen.probe", v, "0");
        enabled = atoi(v);
    }
    if (!enabled || checked >= 12 || !b->stream || b->stream->format == 0x21 || !b->buffer || !*b->buffer)
        return;
    const native_handle_t *h = *b->buffer;
    int fd = h->data[0];
    off_t size = lseek(fd, 0, SEEK_END);
    uint8_t *p = size > 0 ? mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (p == MAP_FAILED) {
        ALOGW("probe: frame %u, mmap failed (size %ld)", frame, (long)size);
        return;
    }
    int nz = 0, n = 0;
    for (off_t o = 0; o < size; o += 4099, n++)
        nz += p[o] != 0;
    ALOGI("probe: frame %u stream %ux%u fmt %#x usage %#x: %d/%d non-zero bytes (size %ld, status %d)", frame,
          b->stream->width, b->stream->height, b->stream->format, b->stream->usage, nz, n, (long)size, b->status);
    munmap(p, size);
    checked++;
}

static void send_buffers(struct tdev *t, uint32_t frame, const camera3_stream_buffer_t *bufs, int n)
{
    for (int i = 0; i < n; i++)
        probe_buffer(frame, &bufs[i]);
    camera3_capture_result_t r;
    memset(&r, 0, sizeof(r));
    r.frame_number = frame;
    r.num_output_buffers = n;
    r.output_buffers = bufs;
    t->kk_cb->process_capture_result(t->kk_cb, &r);
}

/* delivers the pending buffers of the frame, then, in frame order, the complete metadata whose
 * shutter has passed (lock held, released around the KitKat callbacks) */
static void flush_ready(struct tdev *t)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        struct pending *p = &t->pend[i];
        if (p->used && p->nbufs && shuttered(t, p, p->frame)) {
            camera3_stream_buffer_t bufs[MAX_STREAMS];
            int n = p->nbufs;
            uint32_t frame = p->frame;
            memcpy(bufs, p->bufs, n * sizeof(bufs[0]));
            p->nbufs = 0;
            pthread_mutex_unlock(&t->lock);
            send_buffers(t, frame, bufs, n);
            pthread_mutex_lock(&t->lock);
        }
    }
    while (t->next_meta_set) {
        struct pending *p = pending_of(t, t->next_meta, 0);
        if (!p || (!p->ready && !p->skip) || !shuttered(t, p, p->frame) || p->nbufs)
            return;
        camera_metadata_t *md = p->ready;
        uint32_t frame = p->frame;
        p->ready = NULL;
        p->used = 0;
        t->next_meta++;
        if (md) {
            camera3_capture_result_t r;
            memset(&r, 0, sizeof(r));
            r.frame_number = frame;
            r.result = md;
            pthread_mutex_unlock(&t->lock);
            t->kk_cb->process_capture_result(t->kk_cb, &r);
            pthread_mutex_lock(&t->lock);
            free_camera_metadata(md);
        }
    }
}

static void cb_result(const struct a11_callback_ops *cb, const struct a11_capture_result *res)
{
    struct tdev *t = FROM_CB(cb);
    pthread_mutex_lock(&t->lock);
    struct pending *p = NULL;
    if (res->num_output_buffers) {
        camera3_stream_buffer_t bufs[MAX_STREAMS];
        int n = res->num_output_buffers > MAX_STREAMS ? MAX_STREAMS : (int)res->num_output_buffers;
        for (int i = 0; i < n; i++) {
            const struct a11_stream_buffer *b = &res->output_buffers[i];
            bufs[i].stream = kk_stream_of(t, b->stream);
            bufs[i].buffer = b->buffer;
            bufs[i].status = b->status;
            static int warned;
            if (b->status && warned++ < 8)
                ALOGW("frame %u: buffer error on stream %ux%u format %#x", res->frame_number,
                      b->stream ? b->stream->width : 0, b->stream ? b->stream->height : 0, b->stream ? b->stream->format : 0);
            bufs[i].acquire_fence = b->acquire_fence;
            bufs[i].release_fence = b->release_fence;
        }
        p = pending_of(t, res->frame_number, 0);
        if (shuttered(t, p, res->frame_number)) {
            pthread_mutex_unlock(&t->lock);
            send_buffers(t, res->frame_number, bufs, n);
            pthread_mutex_lock(&t->lock);
        } else if ((p = pending_of(t, res->frame_number, 1))) {
            for (int i = 0; i < n && p->nbufs < MAX_STREAMS; i++)
                p->bufs[p->nbufs++] = bufs[i];
        }
    }
    if (res->result && (p = pending_of(t, res->frame_number, 1)) && !p->skip) {
        if (!p->parts) {
            p->parts = A.clone(res->result);
        } else {
            camera_metadata_t *m = A.allocate(A.entry_count(p->parts) + A.entry_count(res->result) + 1,
                                              A.data_count(p->parts) + A.data_count(res->result) + 16);
            A.append(m, p->parts);
            A.append(m, res->result);
            A.free(p->parts);
            p->parts = m;
        }
        if (res->partial_result >= t->partial_count) {
            p->ready = to_kk(p->parts, NULL, 0);
            A.free(p->parts);
            p->parts = NULL;
            flush_ready(t);
        }
    }
    pthread_mutex_unlock(&t->lock);
}

static void cb_notify(const struct a11_callback_ops *cb, const struct a11_notify_msg *msg)
{
    struct tdev *t = FROM_CB(cb);
    camera3_notify_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = msg->type;
    if (msg->type == CAMERA3_MSG_ERROR) {
        uint32_t frame = msg->message.error.frame_number;
        int code = msg->message.error.error_code;
        static int warned;
        if (warned++ < 8)
            ALOGW("frame %u: error %d (stream %p)", frame, code, msg->message.error.error_stream);
        m.message.error.frame_number = frame;
        m.message.error.error_code = code;
        pthread_mutex_lock(&t->lock);
        m.message.error.error_stream = kk_stream_of(t, msg->message.error.error_stream);
        pthread_mutex_unlock(&t->lock);
        t->kk_cb->notify(t->kk_cb, &m);
        if (code == CAMERA3_MSG_ERROR_REQUEST || code == CAMERA3_MSG_ERROR_RESULT) {
            /* no metadata for this frame: stop waiting for it */
            pthread_mutex_lock(&t->lock);
            struct pending *p = pending_of(t, frame, 1);
            if (p) {
                p->skip = 1;
                p->errored |= code == CAMERA3_MSG_ERROR_REQUEST;
                if (p->parts)
                    A.free(p->parts);
                p->parts = NULL;
                if (p->ready)
                    free_camera_metadata(p->ready);
                p->ready = NULL;
                if (!t->next_meta_set) {
                    t->next_meta = frame;
                    t->next_meta_set = 1;
                }
                flush_ready(t);
            }
            pthread_mutex_unlock(&t->lock);
        }
        return;
    }
    m.message.shutter.frame_number = msg->message.shutter.frame_number;
    m.message.shutter.timestamp = msg->message.shutter.timestamp;
    t->kk_cb->notify(t->kk_cb, &m);
    if (msg->type == CAMERA3_MSG_SHUTTER) {
        pthread_mutex_lock(&t->lock);
        t->last_shutter = msg->message.shutter.frame_number;
        t->have_shutter = 1;
        if (!t->next_meta_set) {
            t->next_meta = msg->message.shutter.frame_number;
            t->next_meta_set = 1;
        }
        flush_ready(t);
        pthread_mutex_unlock(&t->lock);
    }
}

static int d_initialize(const struct camera3_device *d, const camera3_callback_ops_t *cb)
{
    struct tdev *t = TDEV(d);
    t->kk_cb = cb;
    t->a11_cb.process_capture_result = cb_result;
    t->a11_cb.notify = cb_notify;
    return t->a->ops->initialize(t->a, &t->a11_cb);
}

static int d_configure_streams(const struct camera3_device *d, camera3_stream_configuration_t *cfg)
{
    struct tdev *t = TDEV(d);
    if (cfg->num_streams > MAX_STREAMS)
        return -EINVAL;
    struct a11_stream *list[MAX_STREAMS];
    pthread_mutex_lock(&t->lock);
    for (uint32_t i = 0; i < cfg->num_streams; i++) {
        camera3_stream_t *s = cfg->streams[i];
        struct a11_stream *a = a11_stream_of(t, s);
        if (!a) {
            for (int k = 0; k < MAX_STREAMS && !a; k++)
                if (!t->streams[k].kk) {
                    a = calloc(1, sizeof(*a));
                    t->streams[k].kk = s;
                    t->streams[k].a = a;
                }
            if (!a) {
                pthread_mutex_unlock(&t->lock);
                return -ENOMEM;
            }
        }
        a->stream_type = s->stream_type;
        a->width = s->width;
        a->height = s->height;
        a->format = s->format;
        a->usage = s->usage;
        a->data_space = s->format == FMT_BLOB ? DATASPACE_V0_JFIF : 0;
        a->rotation = 0;
        a->physical_camera_id = "";
        list[i] = a;
    }
    pthread_mutex_unlock(&t->lock);
    /* session parameters: the HAL only passes the HAL version and capture intent to mm-camera
     * when they are present (otherwise: sensor without a mode, ISP without resources, no stream
     * gets linked). The Android 11 framework fills them with the session keys of the request
     * template: take the HAL default template, the record one if a stream goes to the encoder. */
    int record = 0;
    for (uint32_t i = 0; i < cfg->num_streams; i++)
        record |= (cfg->streams[i]->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) != 0;
    const camera_metadata_t *session = t->a->ops->construct_default_request_settings(
            t->a, record ? CAMERA3_TEMPLATE_VIDEO_RECORD : CAMERA3_TEMPLATE_PREVIEW);
    struct a11_stream_configuration c = { cfg->num_streams, list, 0, session };
    int r = t->a->ops->configure_streams(t->a, &c);
    pthread_mutex_lock(&t->lock);
    for (uint32_t i = 0; i < cfg->num_streams; i++) {
        cfg->streams[i]->usage = list[i]->usage;
        cfg->streams[i]->max_buffers = list[i]->max_buffers;
        ALOGI("stream %ux%u format %#x: usage %#x, %u buffers (%d)", list[i]->width, list[i]->height,
              list[i]->format, list[i]->usage, list[i]->max_buffers, r);
    }
    /* removed streams: the HAL forgot them if the configuration succeeded */
    if (r == 0)
        for (int k = 0; k < MAX_STREAMS; k++) {
            if (!t->streams[k].kk)
                continue;
            int kept = 0;
            for (uint32_t i = 0; i < cfg->num_streams; i++)
                kept |= cfg->streams[i] == t->streams[k].kk;
            if (!kept) {
                free(t->streams[k].a);
                t->streams[k].kk = NULL;
                t->streams[k].a = NULL;
            }
        }
    pthread_mutex_unlock(&t->lock);
    return r;
}

static int d_register_stream_buffers(const struct camera3_device *d, const camera3_stream_buffer_set_t *set)
{
    return 0;  /* useless since HAL3.2: the Android 11 HAL discovers buffers as they come */
}

static const camera_metadata_t *d_default_settings(const struct camera3_device *d, int type)
{
    struct tdev *t = TDEV(d);
    if (type < 0 || type >= 8)
        return NULL;
    if (!t->templates[type]) {
        const camera_metadata_t *m = t->a->ops->construct_default_request_settings(t->a, type);
        if (m)
            t->templates[type] = to_kk(m, NULL, 0);
    }
    return t->templates[type];
}

static int d_process_capture_request(const struct camera3_device *d, camera3_capture_request_t *req)
{
    struct tdev *t = TDEV(d);
    if (req->num_output_buffers > MAX_STREAMS)
        return -EINVAL;
    struct a11_stream_buffer out[MAX_STREAMS], in;
    struct a11_capture_request r;
    memset(&r, 0, sizeof(r));
    r.frame_number = req->frame_number;
    r.num_output_buffers = req->num_output_buffers;
    r.output_buffers = out;
    pthread_mutex_lock(&t->lock);
    for (uint32_t i = 0; i < req->num_output_buffers; i++) {
        const camera3_stream_buffer_t *b = &req->output_buffers[i];
        out[i].stream = a11_stream_of(t, b->stream);
        out[i].buffer = b->buffer;
        out[i].status = b->status;
        out[i].acquire_fence = b->acquire_fence;
        out[i].release_fence = b->release_fence;
    }
    if (req->input_buffer) {
        in.stream = a11_stream_of(t, req->input_buffer->stream);
        in.buffer = req->input_buffer->buffer;
        in.status = req->input_buffer->status;
        in.acquire_fence = req->input_buffer->acquire_fence;
        in.release_fence = req->input_buffer->release_fence;
        r.input_buffer = &in;
    }
    pthread_mutex_unlock(&t->lock);
    camera_metadata_t *settings = NULL;
    if (req->settings) {
        /* HAL template matching the capture intent (same values as templates 1 to 5) */
        camera_metadata_ro_entry_t ce;
        int tmpl = CAMERA3_TEMPLATE_PREVIEW;
        if (find_camera_metadata_ro_entry(req->settings, ANDROID_CONTROL_CAPTURE_INTENT, &ce) == 0 && ce.count &&
            ce.data.u8[0] >= 1 && ce.data.u8[0] <= 5)
            tmpl = ce.data.u8[0];
        settings = to_a11(req->settings, t->a->ops->construct_default_request_settings(t->a, tmpl));
    }
    r.settings = settings;
    int ret = t->a->ops->process_capture_request(t->a, &r);
    if (settings)
        A.free(settings);  /* the HAL copies the settings during the call */
    return ret;
}

static void d_vendor_tag_ops(const struct camera3_device *d, vendor_tag_query_ops_t *ops)
{
    memset(ops, 0, sizeof(*ops));
}

static void d_dump(const struct camera3_device *d, int fd)
{
    struct tdev *t = TDEV(d);
    if (t->a->ops->dump)
        t->a->ops->dump(t->a, fd);
}

static int d_flush(const struct camera3_device *d)
{
    struct tdev *t = TDEV(d);
    return t->a->ops->flush ? t->a->ops->flush(t->a) : 0;
}

static int d_close(hw_device_t *dev)
{
    struct tdev *t = (struct tdev *)dev;
    int r = t->a->common.close(&t->a->common);
    for (int k = 0; k < MAX_STREAMS; k++)
        free(t->streams[k].a);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!t->pend[i].used)
            continue;
        if (t->pend[i].parts)
            A.free(t->pend[i].parts);
        if (t->pend[i].ready)
            free_camera_metadata(t->pend[i].ready);
    }
    for (int i = 0; i < 8; i++)
        if (t->templates[i])
            free_camera_metadata(t->templates[i]);
    pthread_mutex_destroy(&t->lock);
    free(t);
    return r;
}

static int open_camera(const hw_module_t *module, const char *name, hw_device_t **out)
{
    pthread_once(&s_once, load_hal);
    int id = atoi(name);
    if (!s_hal || id < 0 || id >= s_count)
        return -EINVAL;
    hw_device_t *ad = NULL;
    int r = s_hal->common.methods->open(&s_hal->common, name, &ad);
    if (r || !ad) {
        ALOGE("camera %d: open refused (%d)", id, r);
        return r ? r : -ENODEV;
    }
    struct tdev *t = calloc(1, sizeof(*t));
    t->a = (struct a11_device *)ad;
    t->id = id;
    pthread_mutex_init(&t->lock, NULL);
    camera_metadata_ro_entry_t e;
    t->partial_count = A.find_ro(s_a11_info[id].static_camera_characteristics, TAG_PARTIAL_RESULT_COUNT, &e) == 0 &&
                               e.count ? (uint32_t)e.data.i32[0] : 1;
    t->kk_ops.initialize = d_initialize;
    t->kk_ops.configure_streams = d_configure_streams;
    t->kk_ops.register_stream_buffers = d_register_stream_buffers;
    t->kk_ops.construct_default_request_settings = d_default_settings;
    t->kk_ops.process_capture_request = d_process_capture_request;
    t->kk_ops.get_metadata_vendor_tag_ops = d_vendor_tag_ops;
    t->kk_ops.dump = d_dump;
    t->kk_ops.flush = d_flush;
    t->kk.common.tag = HARDWARE_DEVICE_TAG;
    t->kk.common.version = CAMERA_DEVICE_API_VERSION_3_0;
    t->kk.common.module = (hw_module_t *)module;
    t->kk.common.close = d_close;
    t->kk.ops = &t->kk_ops;
    *out = &t->kk.common;
    ALOGI("camera %d opened (results in %u parts)", id, t->partial_count);
    return 0;
}

static struct hw_module_methods_t s_methods = { .open = open_camera };

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_0,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "Taimen (Android 11 Qualcomm HAL through libhyb)",
        .author = "TAIMEN",
        .methods = &s_methods,
    },
    .get_number_of_cameras = get_number_of_cameras,
    .get_camera_info = get_camera_info,
};
