/* nfcbridge, in the Android 11 chroot (64-bit). Loads the NXP NFC HAL (nfc_nci_nxp.so: PN553
 * controller on /dev/nq-nci, firmware download, libnfc-nxp.conf configuration) and publishes it
 * on the abstract socket "nfcbridge" for the KitKat HAL nfc_nci.taimen (see nfcbridge.h). The
 * phNxpNciHal_* functions are C++ functions with plain arguments: called by their mangled names.
 *
 * The PN553 speaks NCI 2.0, the KitKat stack NCI 1.0: the bridge translates the messages that
 * differ (CORE_RESET and CORE_INIT, see xlate_*); the rest of the protocol (RF discovery,
 * activation, data) is common to both versions. */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include "../nfcbridge.h"

#define LOG(...) do { fprintf(stderr, "nfcbridge: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static int (*hal_open)(void (*cb)(uint8_t, uint8_t), void (*data_cb)(uint16_t, uint8_t *));
static int (*hal_write)(uint16_t len, const uint8_t *data);
static int (*hal_core_initialized)(uint8_t *params);
static int (*hal_pre_discover)(void);
static int (*hal_close)(int shutdown);
static int (*hal_control_granted)(void);
static int (*hal_power_cycle)(void);

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;  /* s_client, socket writes */
static int s_trace;  /* nfcbridge --trace: operations, events and NCI packets in the log */

static void trace_nci(const char *dir, const uint8_t *d, unsigned len)
{
    if (!s_trace)
        return;
    char hex[3 * 24 + 4] = "";
    unsigned i;
    for (i = 0; i < len && i < 24; i++)
        sprintf(hex + 3 * i, "%02x ", d[i]);
    if (len > 24)
        strcat(hex, "...");
    LOG("%s %u: %s", dir, len, hex);
}
static int s_client = -1;
static int s_opened;

static void send_msg(uint8_t op, const void *p, uint16_t len)
{
    struct nb_hdr h = { op, 0, len };
    pthread_mutex_lock(&s_lock);
    if (s_client >= 0) {
        struct iovec iov[2] = { { &h, sizeof(h) }, { (void *)p, len } };
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = iov;
        m.msg_iovlen = 2;
        sendmsg(s_client, &m, MSG_NOSIGNAL);
    }
    pthread_mutex_unlock(&s_lock);
}

/* ---------------------------------------------------------------- NCI 2.0 <-> 1.0 */
static pthread_mutex_t s_nci_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_wait_reset_ntf;        /* NCI 2.0 CORE_RESET_RSP received, notification expected */
static uint8_t s_reset_status;
static uint8_t s_mfr_id, s_mfr_info[4];
static uint8_t s_init_rsp2[258];    /* original NCI 2.0 CORE_INIT_RSP, for the HAL's core_initialized */
static unsigned s_init_rsp2_len;

/* command from the stack (1.0) to the controller (2.0); returns the length to write */
static unsigned xlate_cmd(const uint8_t *in, unsigned len, uint8_t *out)
{
    /* CORE_INIT_CMD: 2.0 adds two feature enable bytes */
    if (len == 3 && in[0] == 0x20 && in[1] == 0x01 && in[2] == 0x00) {
        static const uint8_t init2[] = { 0x20, 0x01, 0x02, 0x00, 0x00 };
        memcpy(out, init2, sizeof(init2));
        return sizeof(init2);
    }
    memcpy(out, in, len);
    return len;
}

/* packet from the controller (2.0) to the stack (1.0); returns the length to deliver, 0 to swallow it */
static unsigned xlate_rsp(const uint8_t *in, unsigned len, uint8_t *out)
{
    /* CORE_RESET_RSP 2.0: status only; the version comes in the following notification */
    if (len == 4 && in[0] == 0x40 && in[1] == 0x00 && in[2] == 0x01) {
        s_wait_reset_ntf = 1;
        s_reset_status = in[3];
        return 0;
    }
    /* CORE_RESET_NTF: trigger, configuration status, NCI version, manufacturer, info */
    if (in[0] == 0x60 && in[1] == 0x00 && len >= 8) {
        s_mfr_id = in[6];
        memset(s_mfr_info, 0, sizeof(s_mfr_info));
        memcpy(s_mfr_info, in + 8, in[7] < 4 ? in[7] : 4);
        if (!s_wait_reset_ntf)
            return 0;
        s_wait_reset_ntf = 0;
        /* 1.0 response: status, version (1.0 for the stack), configuration status */
        uint8_t rsp[] = { 0x40, 0x00, 0x03, s_reset_status, 0x10, in[4] };
        memcpy(out, rsp, sizeof(rsp));
        return sizeof(rsp);
    }
    /* CORE_INIT_RSP 2.0 -> 1.0 */
    if (in[0] == 0x40 && in[1] == 0x01 && len >= 3 + 15 && in[3] == 0x00) {
        const uint8_t *p = in + 3;
        s_init_rsp2_len = len < sizeof(s_init_rsp2) ? len : sizeof(s_init_rsp2);
        memcpy(s_init_rsp2, in, s_init_rsp2_len);
        uint8_t n = p[14], *o = out + 3;
        *o++ = p[0];                   /* status */
        memcpy(o, p + 1, 4); o += 4;   /* controller features */
        *o++ = n;                      /* RF interfaces: 2.0 = (interface, extension count, extensions) */
        const uint8_t *q = p + 15;
        for (int i = 0; i < n && q + 1 < in + len; i++) {
            *o++ = q[0];
            q += 2 + q[1];
        }
        *o++ = p[5];                   /* logical connections */
        *o++ = p[6]; *o++ = p[7];      /* routing table */
        *o++ = p[8];                   /* control payload */
        *o++ = 0xff; *o++ = 0x00;      /* large parameters */
        *o++ = s_mfr_id;
        memcpy(o, s_mfr_info, 4); o += 4;
        out[0] = 0x40;
        out[1] = 0x01;
        out[2] = o - (out + 3);
        return o - out;
    }
    memcpy(out, in, len);
    return len;
}

static void on_event(uint8_t event, uint8_t status)
{
    if (s_trace)
        LOG("event %u, status %u", event, status);
    uint8_t e[2] = { event, status };
    send_msg(NB_EVT, e, 2);
}

static void on_data(uint16_t len, uint8_t *data)
{
    trace_nci("NFCC ->", data, len);
    uint8_t out[300];
    pthread_mutex_lock(&s_nci_lock);
    unsigned n = len <= 258 ? xlate_rsp(data, len, out) : 0;
    pthread_mutex_unlock(&s_nci_lock);
    if (len > 258) {
        send_msg(NB_DATA, data, len);
        return;
    }
    if (n) {
        if (n != len || memcmp(out, data, n))
            trace_nci("  translated ->", out, n);
        send_msg(NB_DATA, out, n);
    }
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

static void serve(int cl)
{
    pthread_mutex_lock(&s_lock);
    s_client = cl;
    pthread_mutex_unlock(&s_lock);
    uint8_t buf[1024];
    struct nb_hdr h;
    while (read_full(cl, &h, sizeof(h)) == 0 && h.len <= sizeof(buf) && read_full(cl, buf, h.len) == 0) {
        int32_t r = -EINVAL;
        switch (h.op) {
        case NB_OPEN:
            r = hal_open(on_event, on_data);
            s_opened = r == 0;
            LOG("open: %d", r);
            break;
        case NB_WRITE: {
            uint8_t cmd[sizeof(buf) + 4];
            pthread_mutex_lock(&s_nci_lock);
            unsigned n = xlate_cmd(buf, h.len, cmd);
            pthread_mutex_unlock(&s_nci_lock);
            if (n != h.len)
                trace_nci("  translated -> NFCC", cmd, n);
            r = hal_write(n, cmd) == (int)n ? h.len : -1;
            break;
        }
        case NB_CORE_INITIALIZED:
            /* the NXP HAL expects the original NCI 2.0 CORE_INIT response, not the translation */
            r = hal_core_initialized(s_init_rsp2_len ? s_init_rsp2 : buf);
            break;
        case NB_PRE_DISCOVER:
            r = hal_pre_discover();
            break;
        case NB_CLOSE:
            r = hal_close(0);
            s_opened = 0;
            LOG("close: %d", r);
            break;
        case NB_CONTROL_GRANTED:
            r = hal_control_granted();
            break;
        case NB_POWER_CYCLE:
            r = hal_power_cycle();
            break;
        }
        if (h.op == NB_WRITE || h.op == NB_CORE_INITIALIZED)
            trace_nci(h.op == NB_WRITE ? "-> NFCC" : "core_init", buf, h.len);
        if (s_trace)
            LOG("operation %u: %d", h.op, r);
        send_msg(NB_RET, &r, sizeof(r));
    }
    if (s_opened) {  /* client gone without closing: switch the controller off */
        hal_close(0);
        s_opened = 0;
        LOG("closing after the client left");
    }
    pthread_mutex_lock(&s_lock);
    s_client = -1;
    pthread_mutex_unlock(&s_lock);
}

static void test_event(uint8_t e, uint8_t st) { LOG("event %u, status %u", e, st); }
static void test_data(uint16_t len, uint8_t *d)
{
    char hex[3 * 32 + 1] = "";
    for (int i = 0; i < len && i < 32; i++)
        sprintf(hex + 3 * i, "%02x ", d[i]);
    LOG("NCI received (%u): %s", len, hex);
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    s_trace = argc > 1 && !strcmp(argv[1], "--trace");
    signal(SIGPIPE, SIG_IGN);
    void *lib = dlopen("nfc_nci_nxp.so", RTLD_NOW);
    if (!lib) {
        LOG("nfc_nci_nxp: %s", dlerror());
        return 1;
    }
    hal_open = dlsym(lib, "_Z16phNxpNciHal_openPFvhhEPFvtPhE");
    hal_write = dlsym(lib, "_Z17phNxpNciHal_writetPKh");
    hal_core_initialized = dlsym(lib, "_Z28phNxpNciHal_core_initializedPh");
    hal_pre_discover = dlsym(lib, "_Z24phNxpNciHal_pre_discoverv");
    hal_close = dlsym(lib, "_Z17phNxpNciHal_closeb");
    hal_control_granted = dlsym(lib, "_Z27phNxpNciHal_control_grantedv");
    hal_power_cycle = dlsym(lib, "_Z23phNxpNciHal_power_cyclev");
    if (!hal_open || !hal_write || !hal_core_initialized || !hal_pre_discover || !hal_close ||
        !hal_control_granted || !hal_power_cycle) {
        LOG("NXP HAL functions not found");
        return 1;
    }
    /* nfcbridge --test: opens the HAL directly, events on stderr */
    if (argc > 1 && !strcmp(argv[1], "--test")) {
        LOG("opening the HAL...");
        int r = hal_open(test_event, test_data);
        LOG("open: %d", r);
        sleep(argc > 2 ? atoi(argv[2]) : 10);
        LOG("close: %d", hal_close(0));
        return 0;
    }
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path + 1, NB_SOCKET);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(NB_SOCKET);
    if (bind(srv, (struct sockaddr *)&a, len) || listen(srv, 2)) {
        LOG("socket: %s", strerror(errno));
        return 1;
    }
    LOG("ready");
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
