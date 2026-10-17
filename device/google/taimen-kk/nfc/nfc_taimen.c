/* KitKat NFC HAL (nfc_nci). The NXP PN553 controller is driven by the Android 11 NFC HAL
 * (nfc_nci_nxp.so) in the chroot, published by nfcbridge on the "nfcbridge" socket (see
 * nfcbridge.h): each operation is relayed and waits for its answer; a reader thread delivers
 * events and received NCI packets to the libnfc-nci stack. */
#define LOG_TAG "nfc_nci.taimen"
#include <errno.h>
#include <fcntl.h>
#include <hardware/hardware.h>
#include <hardware/nfc.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <cutils/log.h>
#include "nfcbridge.h"

static nfc_stack_callback_t *s_cback;
static nfc_stack_data_callback_t *s_data_cback;
static int s_sock = -1;
static pthread_t s_reader;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;  /* one operation at a time */
static pthread_mutex_t s_ret_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ret_cond = PTHREAD_COND_INITIALIZER;
static int s_ret_ready, s_dead;
static int32_t s_ret;

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

static void *reader(void *arg)
{
    int fd = (int)(intptr_t)arg;
    uint8_t buf[1024];
    struct nb_hdr h;
    while (read_full(fd, &h, sizeof(h)) == 0 && h.len <= sizeof(buf) && read_full(fd, buf, h.len) == 0) {
        switch (h.op) {
        case NB_RET:
            pthread_mutex_lock(&s_ret_lock);
            memcpy(&s_ret, buf, sizeof(s_ret));
            s_ret_ready = 1;
            pthread_cond_signal(&s_ret_cond);
            pthread_mutex_unlock(&s_ret_lock);
            break;
        case NB_EVT:
            if (s_cback && h.len >= 2)
                s_cback(buf[0], buf[1]);
            break;
        case NB_DATA:
            if (s_data_cback)
                s_data_cback(h.len, buf);
            break;
        }
    }
    ALOGE("nfcbridge disconnected");
    pthread_mutex_lock(&s_ret_lock);
    s_dead = 1;
    pthread_cond_signal(&s_ret_cond);
    pthread_mutex_unlock(&s_ret_lock);
    if (s_cback)
        s_cback(HAL_NFC_ERROR_EVT, HAL_NFC_STATUS_FAILED);
    return NULL;
}

static int bridge_connect(void)
{
    if (s_sock >= 0)
        return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path + 1, NB_SOCKET);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(NB_SOCKET);
    if (connect(fd, (struct sockaddr *)&a, len) < 0) {
        ALOGE("nfcbridge unreachable: %s", strerror(errno));
        close(fd);
        return -1;
    }
    s_sock = fd;
    s_dead = 0;
    pthread_create(&s_reader, NULL, reader, (void *)(intptr_t)fd);
    return 0;
}

static void bridge_disconnect(void)
{
    if (s_sock < 0)
        return;
    shutdown(s_sock, SHUT_RDWR);
    pthread_join(s_reader, NULL);
    close(s_sock);
    s_sock = -1;
}

/* synchronous operation: send, then wait for NB_RET */
static int call(uint8_t op, const void *p, uint16_t len)
{
    pthread_mutex_lock(&s_lock);
    int32_t r = -EPIPE;
    if (s_sock >= 0) {
        struct nb_hdr h = { op, 0, len };
        struct iovec iov[2] = { { &h, sizeof(h) }, { (void *)p, len } };
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = iov;
        m.msg_iovlen = 2;
        pthread_mutex_lock(&s_ret_lock);
        s_ret_ready = 0;
        pthread_mutex_unlock(&s_ret_lock);
        if (sendmsg(s_sock, &m, MSG_NOSIGNAL) == (ssize_t)(sizeof(h) + len)) {
            pthread_mutex_lock(&s_ret_lock);
            while (!s_ret_ready && !s_dead)
                pthread_cond_wait(&s_ret_cond, &s_ret_lock);
            if (s_ret_ready)
                r = s_ret;
            pthread_mutex_unlock(&s_ret_lock);
        }
    }
    pthread_mutex_unlock(&s_lock);
    return r;
}

static int hal_open(const struct nfc_nci_device *dev, nfc_stack_callback_t *cb, nfc_stack_data_callback_t *data_cb)
{
    s_cback = cb;
    s_data_cback = data_cb;
    if (bridge_connect())
        return -ENODEV;
    return call(NB_OPEN, NULL, 0);
}

static int hal_write(const struct nfc_nci_device *dev, uint16_t len, const uint8_t *data)
{
    return call(NB_WRITE, data, len);
}

static int hal_core_initialized(const struct nfc_nci_device *dev, uint8_t *params)
{
    /* params: complete NCI CORE_INIT_RSP message (3-byte header, length in [2]) */
    return call(NB_CORE_INITIALIZED, params, 3 + params[2]);
}

static int hal_pre_discover(const struct nfc_nci_device *dev) { return call(NB_PRE_DISCOVER, NULL, 0); }
static int hal_control_granted(const struct nfc_nci_device *dev) { return call(NB_CONTROL_GRANTED, NULL, 0); }
static int hal_power_cycle(const struct nfc_nci_device *dev) { return call(NB_POWER_CYCLE, NULL, 0); }

static int hal_close(const struct nfc_nci_device *dev)
{
    int r = call(NB_CLOSE, NULL, 0);
    bridge_disconnect();
    return r;
}

static int dev_close(hw_device_t *dev)
{
    bridge_disconnect();
    free(dev);
    return 0;
}

static int nfc_open(const hw_module_t *module, const char *name, hw_device_t **device)
{
    if (strcmp(name, NFC_NCI_CONTROLLER))
        return -EINVAL;
    nfc_nci_device_t *d = calloc(1, sizeof(*d));
    d->common.tag = HARDWARE_DEVICE_TAG;
    d->common.version = 0x00010000;
    d->common.module = (hw_module_t *)module;
    d->common.close = dev_close;
    d->open = hal_open;
    d->write = hal_write;
    d->core_initialized = hal_core_initialized;
    d->pre_discover = hal_pre_discover;
    d->close = hal_close;
    d->control_granted = hal_control_granted;
    d->power_cycle = hal_power_cycle;
    *device = &d->common;
    return 0;
}

static struct hw_module_methods_t s_methods = { .open = nfc_open };

struct nfc_nci_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = 0x0100,
        .hal_api_version = 0x00,
        .id = NFC_NCI_HARDWARE_MODULE_ID,
        .name = "Taimen NFC (NXP PN553 via nfcbridge)",
        .author = "TAIMEN",
        .methods = &s_methods,
    },
};
