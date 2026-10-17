/* KitKat sensor HAL (sensors.taimen). The real sensors are served by sensorbridge, in the
 * Android 11 chroot, on the abstract socket "sensorbridge" (see sensorbridge.h).
 * The daemon may restart: poll() reconnects and enables the active sensors again. */
#define LOG_TAG "sensors.taimen"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <cutils/log.h>
#include <hardware/sensors.h>
#include "sensorbridge.h"

#define MAX_SENSORS 32

static struct sensor_t s_sensors[MAX_SENSORS];
static struct sb_sensor s_raw[MAX_SENSORS];
static int s_count = -1;
static int s_fd = -1;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* flush: KitKat calls it right after batch, before activate, and drops the sensor's events up to
 * the "flush complete" event. The Qualcomm HAL refuses to flush an inactive sensor, so that
 * event is produced here (no hardware FIFO to drain, latency 0). */
static int s_flush_pipe[2] = { -1, -1 };

/* state wanted by the framework, replayed after a reconnection */
static int s_enabled[MAX_SENSORS];
static int64_t s_period[MAX_SENSORS], s_latency[MAX_SENSORS];

static int idx_of(int handle)
{
    for (int i = 0; i < s_count; i++)
        if (s_sensors[i].handle == handle)
            return i;
    return -1;
}

static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t r = read(fd, p, len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        len -= r;
    }
    return 0;
}

static int send_cmd(int fd, uint32_t op, int handle, int64_t a, int64_t b)
{
    struct sb_cmd c = { op, handle, a, b };
    return send(fd, &c, sizeof(c), MSG_NOSIGNAL) == sizeof(c) ? 0 : -1;
}

/* called with s_lock held; returns the descriptor or -1 */
static int connect_locked(int tries)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path + 1, SB_SOCKET);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SB_SOCKET);
    for (int t = 0; t < tries; t++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, len) == 0) {
            uint32_t n;
            if (read_full(fd, &n, 4) == 0 && n <= MAX_SENSORS &&
                read_full(fd, s_raw, sizeof(s_raw[0]) * n) == 0) {
                if (s_count < 0) {
                    s_count = n;
                    for (uint32_t i = 0; i < n; i++) {
                        struct sensor_t *s = &s_sensors[i];
                        s->name = s_raw[i].name;
                        s->vendor = s_raw[i].vendor;
                        s->version = s_raw[i].version;
                        s->handle = s_raw[i].handle;
                        s->type = s_raw[i].type;
                        s->maxRange = s_raw[i].maxRange;
                        s->resolution = s_raw[i].resolution;
                        s->power = s_raw[i].power;
                        s->minDelay = s_raw[i].minDelay;
                        s->fifoReservedEventCount = s_raw[i].fifoReservedEventCount;
                        s->fifoMaxEventCount = s_raw[i].fifoMaxEventCount;
                    }
                }
                /* replay the current state */
                for (int i = 0; i < s_count; i++) {
                    if (!s_enabled[i])
                        continue;
                    send_cmd(fd, SB_BATCH, s_sensors[i].handle, s_period[i], s_latency[i]);
                    send_cmd(fd, SB_ACTIVATE, s_sensors[i].handle, 1, 0);
                }
                ALOGI("connected to sensorbridge (%d sensors)", s_count);
                return fd;
            }
        }
        if (fd >= 0)
            close(fd);
        usleep(500000);
    }
    return -1;
}

static int get_sensors_list(struct sensors_module_t *module, struct sensor_t const **list)
{
    pthread_mutex_lock(&s_lock);
    if (s_count < 0) {
        /* at boot, sensorbridge waits for the SLPI: up to 30 s */
        s_fd = connect_locked(60);
        if (s_count < 0) {
            ALOGE("sensorbridge unreachable: no sensors");
            s_count = 0;
        }
    }
    pthread_mutex_unlock(&s_lock);
    *list = s_sensors;
    return s_count;
}

static int dev_activate(struct sensors_poll_device_t *dev, int handle, int enabled)
{
    pthread_mutex_lock(&s_lock);
    int i = idx_of(handle);
    if (i >= 0) {
        s_enabled[i] = enabled != 0;
        if (s_fd >= 0)
            send_cmd(s_fd, SB_ACTIVATE, handle, enabled != 0, 0);
    }
    pthread_mutex_unlock(&s_lock);
    return i >= 0 ? 0 : -EINVAL;
}

static int dev_set_delay(struct sensors_poll_device_t *dev, int handle, int64_t ns)
{
    pthread_mutex_lock(&s_lock);
    int i = idx_of(handle);
    if (i >= 0) {
        s_period[i] = ns;
        s_latency[i] = 0;
        if (s_fd >= 0)
            send_cmd(s_fd, SB_SET_DELAY, handle, ns, 0);
    }
    pthread_mutex_unlock(&s_lock);
    return i >= 0 ? 0 : -EINVAL;
}

static int dev_batch(struct sensors_poll_device_1 *dev, int handle, int flags, int64_t period, int64_t timeout)
{
    if (flags & SENSORS_BATCH_DRY_RUN)
        return idx_of(handle) >= 0 ? 0 : -EINVAL;
    pthread_mutex_lock(&s_lock);
    int i = idx_of(handle);
    if (i >= 0) {
        s_period[i] = period;
        s_latency[i] = timeout;
        if (s_fd >= 0)
            send_cmd(s_fd, SB_BATCH, handle, period, timeout);
    }
    pthread_mutex_unlock(&s_lock);
    return i >= 0 ? 0 : -EINVAL;
}

static int dev_flush(struct sensors_poll_device_1 *dev, int handle)
{
    if (idx_of(handle) < 0)
        return -EINVAL;
    int32_t h = handle;
    return write(s_flush_pipe[1], &h, sizeof(h)) == sizeof(h) ? 0 : -errno;
}

static int flush_events(sensors_event_t *data, int count)
{
    int n = 0;
    int32_t h;
    while (n < count && read(s_flush_pipe[0], &h, sizeof(h)) == sizeof(h)) {
        memset(&data[n], 0, sizeof(data[n]));
        data[n].version = META_DATA_VERSION;
        data[n].type = SENSOR_TYPE_META_DATA;
        data[n].meta_data.what = META_DATA_FLUSH_COMPLETE;
        data[n].meta_data.sensor = h;
        n++;
    }
    return n;
}

static int dev_poll(struct sensors_poll_device_t *dev, sensors_event_t *data, int count)
{
    for (;;) {
        pthread_mutex_lock(&s_lock);
        int fd = s_fd;
        pthread_mutex_unlock(&s_lock);
        if (fd < 0) {
            pthread_mutex_lock(&s_lock);
            if (s_fd < 0)
                s_fd = connect_locked(1);
            fd = s_fd;
            pthread_mutex_unlock(&s_lock);
            if (fd < 0) {
                sleep(1);
                continue;
            }
        }
        /* wait for events from the bridge or a flush completion to report */
        struct pollfd pfd[2] = { { fd, POLLIN, 0 }, { s_flush_pipe[0], POLLIN, 0 } };
        if (poll(pfd, 2, -1) < 0 && errno != EINTR)
            continue;
        int nf = flush_events(data, count);
        if (nf > 0)
            return nf;
        if (!(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        /* at least one event, then whatever already arrived, without blocking */
        if (read_full(fd, &data[0], SB_EVENT_SIZE) == 0) {
            int n = 1;
            while (n < count) {
                ssize_t r = recv(fd, &data[n], SB_EVENT_SIZE, MSG_DONTWAIT | MSG_PEEK);
                if (r != SB_EVENT_SIZE || read_full(fd, &data[n], SB_EVENT_SIZE))
                    break;
                n++;
            }
            return n;
        }
        ALOGE("sensorbridge disconnected, reconnecting");
        pthread_mutex_lock(&s_lock);
        if (s_fd == fd) {
            close(s_fd);
            s_fd = -1;
        }
        pthread_mutex_unlock(&s_lock);
    }
}

static int dev_close(struct hw_device_t *dev)
{
    free(dev);
    return 0;
}

static int open_sensors(const struct hw_module_t *module, const char *id, struct hw_device_t **device)
{
    if (s_flush_pipe[0] < 0) {
        pipe(s_flush_pipe);
        fcntl(s_flush_pipe[0], F_SETFL, O_NONBLOCK);
    }
    struct sensors_poll_device_1 *dev = calloc(1, sizeof(*dev));
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = SENSORS_DEVICE_API_VERSION_1_1;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = dev_close;
    dev->activate = dev_activate;
    dev->setDelay = dev_set_delay;
    dev->poll = dev_poll;
    dev->batch = dev_batch;
    dev->flush = dev_flush;
    *device = &dev->common;
    return 0;
}

static struct hw_module_methods_t sensors_methods = { .open = open_sensors };

struct sensors_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = SENSORS_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = SENSORS_HARDWARE_MODULE_ID,
        .name = "Pixel 2 XL (Android 11 SLPI bridge)",
        .author = "taimen-kk",
        .methods = &sensors_methods,
    },
    .get_sensors_list = get_sensors_list,
};
