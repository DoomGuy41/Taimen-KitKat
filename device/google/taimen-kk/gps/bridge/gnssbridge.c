/* gnssbridge, in the Android 11 chroot (64-bit). Client of the modem's QMI LOC service (GNSS
 * engine), published on the abstract socket "gnssbridge" for the KitKat HAL gps.taimen (see
 * gnssbridge.h).
 * QMI messages are encoded and decoded by hand (TLV), following the LOC service description of
 * libqmi: libqmi_cci carries them as is (qmi_client_send_raw_msg_sync, raw indications), which
 * avoids libloc_api_v02 and the whole Qualcomm location stack. */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "../gnssbridge.h"

#define LOG(...) do { fprintf(stderr, "gnssbridge: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ---- libqmi_cci (qmi_client.h) ---- */
typedef void *qmi_client_type;
typedef void (*qmi_client_ind_cb)(qmi_client_type h, unsigned int msg_id, void *buf, unsigned int len,
                                  void *data);
/* qmi_idl_service_object: the encoding tables are left out, useless in raw mode. Values taken
 * from loc_qmi_idl_service_object_v02 (libloc_api_v02.so of the factory image). */
struct qmi_idl_service_object {
    uint32_t library_version;
    uint32_t idl_version;
    uint32_t service_id;
    uint32_t max_msg_len;
    uint16_t n_msgs[3];
    const void *msgid_to_msg[3];
    const void *p_type_table;
    uint32_t idl_minor_version;
    struct qmi_idl_service_object *parent_service_obj;
};
static struct qmi_idl_service_object s_loc_svc = { 6, 2, 0x10, 0x200c, { 0, 0, 0 }, { 0, 0, 0 }, 0, 66, 0 };

static int (*p_init_instance)(void *svc, int instance, qmi_client_ind_cb cb, void *cb_data, void *os_params,
                              uint32_t timeout, qmi_client_type *h);
static int (*p_send_raw_sync)(qmi_client_type h, unsigned int msg_id, void *req, unsigned int req_len,
                              void *resp, unsigned int resp_len, unsigned int *resp_got, unsigned int timeout);
#define QMI_CLIENT_INSTANCE_ANY 0xffff

/* ---- LOC service ---- */
#define LOC_REG_EVENTS 0x0021
#define LOC_START 0x0022
#define LOC_STOP 0x0023
#define LOC_IND_POSITION 0x0024
#define LOC_IND_SV_INFO 0x0025
#define LOC_IND_NMEA 0x0026
#define LOC_IND_ENGINE_STATE 0x002B
#define LOC_IND_XTRA_REQUEST 0x0029
#define LOC_INJECT_XTRA 0x0035
#define LOC_INJECT_UTC_TIME 0x0038
#define LOC_INJECT_POSITION 0x0039
#define LOC_SET_ENGINE_LOCK 0x003A
#define LOC_SET_OPERATION_MODE 0x004A

#define EV_POSITION (1ull << 0)
#define EV_SV_INFO (1ull << 1)
#define EV_NMEA (1ull << 2)
#define EV_XTRA_REQUEST (1ull << 5)
#define EV_ENGINE_STATE (1ull << 7)

#define SESSION_ID 1

static qmi_client_type s_qmi;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;  /* s_client, s_used */
static int s_client = -1;
static uint16_t s_used[GB_MAX_SVS];
static int s_nused;
static int s_running;

/* ---------------------------------------------------------------- TLV encoding */
struct tlvbuf {
    uint8_t b[512];
    unsigned n;
};

static void tlv(struct tlvbuf *t, uint8_t type, const void *v, uint16_t len)
{
    t->b[t->n] = type;
    t->b[t->n + 1] = len & 0xff;
    t->b[t->n + 2] = len >> 8;
    memcpy(t->b + t->n + 3, v, len);
    t->n += 3 + len;
}

static void tlv_u8(struct tlvbuf *t, uint8_t type, uint8_t v) { tlv(t, type, &v, 1); }
static void tlv_u32(struct tlvbuf *t, uint8_t type, uint32_t v) { tlv(t, type, &v, 4); }
static void tlv_u64(struct tlvbuf *t, uint8_t type, uint64_t v) { tlv(t, type, &v, 8); }

/* looks for TLV type in buf; returns its value and length */
static const uint8_t *find_tlv(const uint8_t *buf, unsigned len, uint8_t type, unsigned *vlen)
{
    unsigned i = 0;
    while (i + 3 <= len) {
        unsigned l = buf[i + 1] | buf[i + 2] << 8;
        if (i + 3 + l > len)
            break;
        if (buf[i] == type) {
            *vlen = l;
            return buf + i + 3;
        }
        i += 3 + l;
    }
    return NULL;
}

#define GET(buf, len, type, var) ({                                   \
    unsigned _l;                                                      \
    const uint8_t *_v = find_tlv(buf, len, type, &_l);                \
    int _ok = _v && _l >= sizeof(var);                                \
    if (_ok)                                                          \
        memcpy(&(var), _v, sizeof(var));                              \
    _ok; })

/* synchronous request: 0 if the modem accepts it (TLV 0x02: result, error) */
static int request(unsigned msg_id, struct tlvbuf *t, const char *what)
{
    uint8_t resp[64];
    unsigned got = 0;
    int r = p_send_raw_sync(s_qmi, msg_id, t->b, t->n, resp, sizeof(resp), &got, 5000);
    if (r) {
        LOG("%s: QMI error %d", what, r);
        return -1;
    }
    uint16_t res[2] = { 1, 0 };
    if (!GET(resp, got, 0x02, res) || res[0]) {
        LOG("%s: refused (error %u)", what, res[1]);
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- to the client */
static void send_msg(uint16_t type, const void *a, size_t alen, const void *b, size_t blen)
{
    struct gb_hdr h = { type, (uint16_t)(alen + blen) };
    pthread_mutex_lock(&s_lock);
    if (s_client >= 0) {
        struct iovec iov[3] = { { &h, sizeof(h) }, { (void *)a, alen }, { (void *)b, blen } };
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = iov;
        m.msg_iovlen = 3;
        sendmsg(s_client, &m, MSG_NOSIGNAL);
    }
    pthread_mutex_unlock(&s_lock);
}

static void send_status(uint32_t st) { send_msg(GB_STATUS, &st, sizeof(st), NULL, 0); }

static int64_t now_utc_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- indications */
static void on_position(const uint8_t *buf, unsigned len)
{
    uint32_t status = 1;
    double lat, lon;
    if (!GET(buf, len, 0x01, status) || status != 0)  /* 0: fix succeeded */
        return;
    if (!GET(buf, len, 0x10, lat) || !GET(buf, len, 0x11, lon))
        return;
    struct gb_location l;
    memset(&l, 0, sizeof(l));
    l.flags = GB_HAS_LAT_LONG;
    l.latitude = lat;
    l.longitude = lon;
    float f;
    if (GET(buf, len, 0x1A, f)) {
        l.altitude = f;
        l.flags |= GB_HAS_ALTITUDE;
    }
    if (GET(buf, len, 0x12, f)) {
        l.accuracy = f;
        l.flags |= GB_HAS_ACCURACY;
    }
    if (GET(buf, len, 0x18, f)) {
        l.speed = f;
        l.flags |= GB_HAS_SPEED;
    }
    if (GET(buf, len, 0x20, f)) {
        l.bearing = f;
        l.flags |= GB_HAS_BEARING;
    }
    uint64_t utc;
    l.utc_ms = GET(buf, len, 0x25, utc) ? (int64_t)utc : now_utc_ms();

    unsigned ul;
    const uint8_t *u = find_tlv(buf, len, 0x2C, &ul);
    pthread_mutex_lock(&s_lock);
    s_nused = 0;
    if (u && ul >= 1)
        for (unsigned i = 0; i < u[0] && 1 + 2 * i + 2 <= ul && s_nused < GB_MAX_SVS; i++)
            s_used[s_nused++] = u[1 + 2 * i] | u[2 + 2 * i] << 8;
    pthread_mutex_unlock(&s_lock);
    send_msg(GB_LOCATION, &l, sizeof(l), NULL, 0);
}

static void on_sv_info(const uint8_t *buf, unsigned len)
{
    /* TLV 0x10: uint8 count, then compact 28-byte elements */
    unsigned l;
    const uint8_t *v = find_tlv(buf, len, 0x10, &l);
    uint32_t n = 0;
    struct gb_sv sv[GB_MAX_SVS];
    if (v && l >= 1) {
        pthread_mutex_lock(&s_lock);
        for (unsigned i = 0; i < v[0] && 1 + 28 * (i + 1) <= l && n < GB_MAX_SVS; i++) {
            const uint8_t *e = v + 1 + 28 * i;
            uint32_t valid, system, state;
            uint16_t id;
            memcpy(&valid, e, 4);
            memcpy(&system, e + 4, 4);
            memcpy(&id, e + 8, 2);
            memcpy(&state, e + 11, 4);
            uint8_t nav = e[15];
            struct gb_sv *s = &sv[n];
            memset(s, 0, sizeof(*s));
            s->prn = id;
            s->system = system;
            memcpy(&s->elevation, e + 16, 4);
            memcpy(&s->azimuth, e + 20, 4);
            memcpy(&s->snr, e + 24, 4);
            /* the state (idle 1, search 2, track 3 for Qualcomm, shifted by 1 in libqmi) is not
             * used: a satellite is tracked as soon as it has a valid non-zero SNR */
            (void)state;
            if (!(valid & (1 << 7)))
                s->snr = 0;
            if (valid & (1 << 4)) {  /* info mask: 0x01 ephemeris, 0x02 almanac */
                if (nav & 1)
                    s->flags |= GB_SV_EPHEMERIS;
                if (nav & 2)
                    s->flags |= GB_SV_ALMANAC;
            }
            for (int k = 0; k < s_nused; k++)
                if (s_used[k] == id)
                    s->flags |= GB_SV_USED;
            n++;
        }
        pthread_mutex_unlock(&s_lock);
    }
    send_msg(GB_SVSTATUS, &n, sizeof(n), sv, n * sizeof(sv[0]));
}

static void on_ind(qmi_client_type h, unsigned int msg_id, void *ind, unsigned int len, void *data)
{
    (void)h;
    (void)data;
    const uint8_t *b = ind;
    switch (msg_id) {
    case LOC_IND_POSITION:
        on_position(b, len);
        break;
    case LOC_IND_SV_INFO:
        on_sv_info(b, len);
        break;
    case LOC_IND_NMEA: {
        unsigned l;
        const uint8_t *s = find_tlv(b, len, 0x01, &l);
        if (s) {
            while (l > 0 && (s[l - 1] == 0 || s[l - 1] == '\n' || s[l - 1] == '\r'))
                l--;
            int64_t t = now_utc_ms();
            send_msg(GB_NMEA, &t, sizeof(t), s, l);
        }
        break;
    }
    case LOC_IND_XTRA_REQUEST:
        LOG("the modem asks for predicted orbits (XTRA)");
        send_msg(GB_XTRA_REQUEST, NULL, 0, NULL, 0);
        break;
    case LOC_IND_ENGINE_STATE: {
        int32_t st;
        if (GET(b, len, 0x01, st))
            send_status(st == 1 ? 3 : 4);
        break;
    }
    }
}

/* ---------------------------------------------------------------- commands */
static void loc_start(uint32_t interval_ms)
{
    struct tlvbuf t = { .n = 0 };
    tlv_u8(&t, 0x01, SESSION_ID);
    tlv_u32(&t, 0x10, 1);  /* periodic fixes */
    tlv_u32(&t, 0x12, 2);  /* no intermediate reports */
    tlv_u32(&t, 0x13, interval_ms ? interval_ms : 1000);
    if (request(LOC_START, &t, "start") == 0) {
        s_running = 1;
        send_status(1);
        LOG("session started (%u ms)", interval_ms);
    }
}

static void loc_stop(void)
{
    if (!s_running)
        return;
    struct tlvbuf t = { .n = 0 };
    tlv_u8(&t, 0x01, SESSION_ID);
    request(LOC_STOP, &t, "stop");
    s_running = 0;
    send_status(2);
    LOG("session stopped");
}

static void handle(const struct gb_cmd *c)
{
    struct tlvbuf t = { .n = 0 };
    switch (c->op) {
    case GB_START:
        loc_start(c->arg);
        break;
    case GB_STOP:
        loc_stop();
        break;
    case GB_INJECT_TIME:
        tlv_u64(&t, 0x01, (uint64_t)c->a);
        tlv_u32(&t, 0x02, c->arg);
        request(LOC_INJECT_UTC_TIME, &t, "time injection");
        break;
    case GB_INJECT_LOCATION: {
        double lat = c->a / 1e7, lon = c->b / 1e7;
        float acc = c->arg ? c->arg : 1000;
        tlv(&t, 0x10, &lat, 8);
        tlv(&t, 0x11, &lon, 8);
        tlv(&t, 0x12, &acc, 4);
        request(LOC_INJECT_POSITION, &t, "position injection");
        break;
    }
    }
}

/* XTRA predicted orbits, injected in 1024-byte parts */
static void inject_xtra(const uint8_t *data, uint32_t size)
{
    uint16_t parts = (size + 1023) / 1024;
    for (uint16_t i = 0; i < parts; i++) {
        uint16_t n = size - i * 1024u < 1024 ? size - i * 1024u : 1024;
        static uint8_t req[1100];
        unsigned o = 0;
        req[o++] = 0x01; req[o++] = 4; req[o++] = 0; memcpy(req + o, &size, 4); o += 4;
        req[o++] = 0x02; req[o++] = 2; req[o++] = 0; memcpy(req + o, &parts, 2); o += 2;
        uint16_t num = i + 1;
        req[o++] = 0x03; req[o++] = 2; req[o++] = 0; memcpy(req + o, &num, 2); o += 2;
        uint16_t tl = n + 2;
        req[o++] = 0x04; req[o++] = tl & 0xff; req[o++] = tl >> 8; memcpy(req + o, &n, 2); o += 2;
        memcpy(req + o, data + i * 1024u, n); o += n;
        uint32_t fmt = 0;  /* XTRA */
        req[o++] = 0x10; req[o++] = 4; req[o++] = 0; memcpy(req + o, &fmt, 4); o += 4;
        uint8_t resp[64];
        unsigned got = 0;
        uint16_t res[2] = { 1, 0 };
        if (p_send_raw_sync(s_qmi, LOC_INJECT_XTRA, req, o, resp, sizeof(resp), &got, 5000) ||
            !GET(resp, got, 0x02, res) || res[0]) {
            LOG("XTRA: part %u/%u refused (error %u)", num, parts, res[1]);
            return;
        }
    }
    LOG("XTRA: %u bytes injected in %u parts", size, parts);
}

static int qmi_setup(void)
{
    void *lib = dlopen("libqmi_cci.so", RTLD_NOW);
    if (!lib) {
        LOG("libqmi_cci: %s", dlerror());
        return -1;
    }
    p_init_instance = dlsym(lib, "qmi_client_init_instance");
    p_send_raw_sync = dlsym(lib, "qmi_client_send_raw_msg_sync");
    if (!p_init_instance || !p_send_raw_sync)
        return -1;
    static uint64_t os_params[64];  /* qmi_client_os_params: cond + mutex, initialized by cci */
    /* the LOC service only shows up once the modem has booted: wait for it as long as needed */
    for (int i = 0;; i++) {
        int r = p_init_instance(&s_loc_svc, QMI_CLIENT_INSTANCE_ANY, on_ind, NULL, os_params, 10000, &s_qmi);
        if (r == 0)
            break;
        if (i % 30 == 0)
            LOG("LOC service unavailable (%d), waiting", r);
        sleep(2);
    }
    if (!s_qmi)
        return -1;

    struct tlvbuf t = { .n = 0 };
    tlv_u64(&t, 0x01, EV_POSITION | EV_SV_INFO | EV_NMEA | EV_ENGINE_STATE | EV_XTRA_REQUEST);
    if (request(LOC_REG_EVENTS, &t, "event registration"))
        return -1;
    t.n = 0;
    tlv_u32(&t, 0x01, 1);  /* engine unlocked */
    request(LOC_SET_ENGINE_LOCK, &t, "engine unlock");
    t.n = 0;
    tlv_u32(&t, 0x01, 4);  /* standalone: no SUPL server configured */
    request(LOC_SET_OPERATION_MODE, &t, "standalone mode");
    LOG("LOC service ready");
    return 0;
}

static void serve(int cl)
{
    pthread_mutex_lock(&s_lock);
    s_client = cl;
    pthread_mutex_unlock(&s_lock);
    for (;;) {
        struct gb_cmd c;
        size_t got = 0;
        while (got < sizeof(c)) {
            ssize_t n = read(cl, (char *)&c + got, sizeof(c) - got);
            if (n <= 0)
                goto out;
            got += n;
        }
        if (c.op == GB_INJECT_XTRA) {  /* the file follows the command */
            if (c.arg > GB_MAX_XTRA)
                goto out;
            static uint8_t xtra[GB_MAX_XTRA];
            for (got = 0; got < c.arg;) {
                ssize_t n = read(cl, xtra + got, c.arg - got);
                if (n <= 0)
                    goto out;
                got += n;
            }
            inject_xtra(xtra, c.arg);
            continue;
        }
        handle(&c);
    }
out:
    loc_stop();
    pthread_mutex_lock(&s_lock);
    s_client = -1;
    pthread_mutex_unlock(&s_lock);
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (qmi_setup())
        return 1;

    /* gnssbridge --test [seconds]: direct session, reports on stderr */
    if (argc > 1 && !strcmp(argv[1], "--test")) {
        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        s_client = fds[0];
        loc_start(1000);
        int secs = argc > 2 ? atoi(argv[2]) : 60;
        time_t end = time(NULL) + secs;
        while (time(NULL) < end) {
            struct gb_hdr h;
            uint8_t p[2048];
            if (read(fds[1], &h, sizeof(h)) != sizeof(h) || read(fds[1], p, h.len) != h.len)
                break;
            if (h.type == GB_NMEA)
                LOG("NMEA %.*s", h.len - 8, p + 8);
            else if (h.type == GB_SVSTATUS) {
                uint32_t n;
                memcpy(&n, p, 4);
                struct gb_sv *sv = (struct gb_sv *)(p + 4);
                int tracked = 0;
                for (uint32_t i = 0; i < n; i++)
                    tracked += sv[i].snr > 0;
                LOG("satellites: %u known, %d tracked", n, tracked);
            } else if (h.type == GB_LOCATION) {
                struct gb_location *l = (struct gb_location *)p;
                LOG("FIX %.6f %.6f +/-%.0f m", l->latitude, l->longitude, l->accuracy);
            } else if (h.type == GB_STATUS)
                LOG("status %u", *(uint32_t *)p);
        }
        loc_stop();
        return 0;
    }

    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path + 1, GB_SOCKET);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(GB_SOCKET);
    if (bind(srv, (struct sockaddr *)&a, len) || listen(srv, 2)) {
        LOG("socket: %s", strerror(errno));
        return 1;
    }
    for (;;) {
        int cl = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
        if (cl < 0)
            continue;
        LOG("client connected");
        serve(cl);
        close(cl);
        LOG("client disconnected");
    }
}
