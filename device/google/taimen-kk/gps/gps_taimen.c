/* KitKat GPS HAL. The GNSS engine is in the modem; gnssbridge (Android 11 chroot) is its QMI LOC
 * client and publishes fixes, satellites and NMEA on the "gnssbridge" socket (see gnssbridge.h).
 * This HAL connects to it and relays to the framework. */
#define LOG_TAG "gps.taimen"
#include <errno.h>
#include <fcntl.h>
#include <hardware/gps.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cutils/log.h>
#include "gnssbridge.h"

static GpsCallbacks s_cb;
static GpsXtraCallbacks s_xtra_cb;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;  /* s_sock, s_started, s_interval */
static int s_sock = -1;
static int s_started;
static uint32_t s_interval = 1000;
/* each init() starts a reader of generation s_gen; cleanup() changes it: the old reader stops
 * even if init() is called again right away (GPS switched off and on in Settings) */
static volatile int s_gen;

static int send_cmd(uint32_t op, uint32_t arg, int64_t a, int64_t b)
{
    struct gb_cmd c = { op, arg, a, b };
    int r = -1;
    pthread_mutex_lock(&s_lock);
    if (s_sock >= 0)
        r = send(s_sock, &c, sizeof(c), MSG_NOSIGNAL) == sizeof(c) ? 0 : -1;
    pthread_mutex_unlock(&s_lock);
    return r;
}

static int connect_bridge(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path + 1, GB_SOCKET);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(GB_SOCKET);
    if (connect(fd, (struct sockaddr *)&a, len) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0)
            return -1;
        got += n;
    }
    return 0;
}

static void on_location(const struct gb_location *l)
{
    GpsLocation g;
    memset(&g, 0, sizeof(g));
    g.size = sizeof(g);
    g.flags = l->flags;
    g.latitude = l->latitude;
    g.longitude = l->longitude;
    g.altitude = l->altitude;
    g.speed = l->speed;
    g.bearing = l->bearing;
    g.accuracy = l->accuracy;
    g.timestamp = l->utc_ms;
    s_cb.location_cb(&g);
}

/* KitKat: 32 satellites at most, masks indexed by GPS PRN (1 to 32); GPS first */
static void on_svstatus(const uint8_t *p, size_t len)
{
    uint32_t n;
    memcpy(&n, p, 4);
    if (4 + n * sizeof(struct gb_sv) > len)
        return;
    GpsSvStatus s;
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n && s.num_svs < GPS_MAX_SVS; i++) {
            struct gb_sv sv;
            memcpy(&sv, p + 4 + i * sizeof(sv), sizeof(sv));
            int gps = sv.prn >= 1 && sv.prn <= 32;
            if (gps != (pass == 0))
                continue;
            GpsSvInfo *o = &s.sv_list[s.num_svs++];
            o->size = sizeof(*o);
            o->prn = sv.prn;
            o->snr = sv.snr;
            o->elevation = sv.elevation;
            o->azimuth = sv.azimuth;
            if (gps) {
                uint32_t bit = 1u << (sv.prn - 1);
                if (sv.flags & GB_SV_EPHEMERIS)
                    s.ephemeris_mask |= bit;
                if (sv.flags & GB_SV_ALMANAC)
                    s.almanac_mask |= bit;
                if (sv.flags & GB_SV_USED)
                    s.used_in_fix_mask |= bit;
            }
        }
    s_cb.sv_status_cb(&s);
}

static void reader(void *arg)
{
    int gen = (int)(intptr_t)arg;
    uint8_t p[4096];
    while (gen == s_gen) {
        int fd = connect_bridge();
        if (fd < 0) {
            sleep(2);
            continue;
        }
        pthread_mutex_lock(&s_lock);
        if (gen != s_gen) {
            pthread_mutex_unlock(&s_lock);
            close(fd);
            break;
        }
        ALOGI("connected to gnssbridge");
        s_sock = fd;
        int started = s_started;
        pthread_mutex_unlock(&s_lock);
        if (started)  /* gnssbridge restarted during a session: resume it */
            send_cmd(GB_START, s_interval, 0, 0);
        struct gb_hdr h;
        while (gen == s_gen && read_full(fd, &h, sizeof(h)) == 0 && read_full(fd, p, h.len) == 0) {
            switch (h.type) {
            case GB_LOCATION:
                if (h.len >= sizeof(struct gb_location))
                    on_location((const struct gb_location *)p);
                break;
            case GB_SVSTATUS:
                if (h.len >= 4)
                    on_svstatus(p, h.len);
                break;
            case GB_NMEA:
                if (h.len > 8) {
                    int64_t t;
                    memcpy(&t, p, 8);
                    s_cb.nmea_cb(t, (const char *)p + 8, h.len - 8);
                }
                break;
            case GB_XTRA_REQUEST:
                if (s_xtra_cb.download_request_cb)
                    s_xtra_cb.download_request_cb();
                break;
            case GB_STATUS:
                if (h.len >= 4) {
                    GpsStatus st = { sizeof(st), 0 };
                    uint32_t v;
                    memcpy(&v, p, 4);
                    st.status = v;
                    s_cb.status_cb(&st);
                }
                break;
            }
        }
        pthread_mutex_lock(&s_lock);
        if (s_sock == fd)
            s_sock = -1;
        pthread_mutex_unlock(&s_lock);
        close(fd);
        ALOGW("gnssbridge disconnected");
    }
}

static int gps_init(GpsCallbacks *cb)
{
    s_cb = *cb;
    pthread_mutex_lock(&s_lock);
    int gen = ++s_gen;
    pthread_mutex_unlock(&s_lock);
    s_cb.set_capabilities_cb(GPS_CAPABILITY_SCHEDULING);
    if (!s_cb.create_thread_cb("gps.taimen", reader, (void *)(intptr_t)gen))
        return -1;
    return 0;
}

static int gps_start(void)
{
    pthread_mutex_lock(&s_lock);
    s_started = 1;
    pthread_mutex_unlock(&s_lock);
    return send_cmd(GB_START, s_interval, 0, 0) == 0 ? 0 : -1;
}

static int gps_stop(void)
{
    pthread_mutex_lock(&s_lock);
    s_started = 0;
    pthread_mutex_unlock(&s_lock);
    send_cmd(GB_STOP, 0, 0, 0);
    return 0;
}

static void gps_cleanup(void)
{
    pthread_mutex_lock(&s_lock);
    s_gen++;
    if (s_sock >= 0)
        shutdown(s_sock, SHUT_RDWR);
    pthread_mutex_unlock(&s_lock);
}

static int gps_inject_time(GpsUtcTime time, int64_t ref, int uncertainty)
{
    (void)ref;
    return send_cmd(GB_INJECT_TIME, uncertainty, time, 0);
}

static int gps_inject_location(double lat, double lon, float accuracy)
{
    return send_cmd(GB_INJECT_LOCATION, (uint32_t)accuracy, (int64_t)(lat * 1e7), (int64_t)(lon * 1e7));
}

static void gps_delete_aiding_data(GpsAidingData flags) { (void)flags; }

static int gps_set_position_mode(GpsPositionMode mode, GpsPositionRecurrence rec, uint32_t min_interval,
                                 uint32_t pref_accuracy, uint32_t pref_time)
{
    (void)mode;
    (void)rec;
    (void)pref_accuracy;
    (void)pref_time;
    pthread_mutex_lock(&s_lock);
    s_interval = min_interval ? min_interval : 1000;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

/* XTRA predicted orbits: the framework downloads the file (XTRA_SERVER_n of gps.conf) when the
 * modem asks for it, then injects it here */
static int xtra_init(GpsXtraCallbacks *cb)
{
    s_xtra_cb = *cb;
    return 0;
}

static int xtra_inject(char *data, int length)
{
    if (length <= 0 || length > GB_MAX_XTRA)
        return -1;
    struct gb_cmd c = { GB_INJECT_XTRA, (uint32_t)length, 0, 0 };
    int r = -1;
    pthread_mutex_lock(&s_lock);
    if (s_sock >= 0 && send(s_sock, &c, sizeof(c), MSG_NOSIGNAL) == sizeof(c)) {
        int off = 0;
        while (off < length) {
            ssize_t n = send(s_sock, data + off, length - off, MSG_NOSIGNAL);
            if (n <= 0)
                break;
            off += n;
        }
        r = off == length ? 0 : -1;
    }
    pthread_mutex_unlock(&s_lock);
    ALOGI("XTRA: %d bytes sent (%d)", length, r);
    return r;
}

static const GpsXtraInterface s_xtra = { sizeof(GpsXtraInterface), xtra_init, xtra_inject };

static const void *gps_get_extension(const char *name)
{
    if (name && !strcmp(name, GPS_XTRA_INTERFACE))
        return &s_xtra;
    return NULL;
}

static const GpsInterface s_iface = {
    sizeof(GpsInterface),
    gps_init,
    gps_start,
    gps_stop,
    gps_cleanup,
    gps_inject_time,
    gps_inject_location,
    gps_delete_aiding_data,
    gps_set_position_mode,
    gps_get_extension,
};

static const GpsInterface *get_gps_interface(struct gps_device_t *dev)
{
    (void)dev;
    return &s_iface;
}

static int open_gps(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    (void)id;
    static struct gps_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.common.tag = HARDWARE_DEVICE_TAG;
    dev.common.version = 0;
    dev.common.module = (struct hw_module_t *)module;
    dev.get_gps_interface = get_gps_interface;
    *device = &dev.common;
    return 0;
}

static struct hw_module_methods_t gps_methods = { .open = open_gps };

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = GPS_HARDWARE_MODULE_ID,
    .name = "Taimen GNSS (QMI LOC via gnssbridge)",
    .author = "TAIMEN",
    .methods = &gps_methods,
};
