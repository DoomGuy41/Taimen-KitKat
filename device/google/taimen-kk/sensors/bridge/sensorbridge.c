/* sensorbridge, in the Android 11 chroot (64-bit). Loads the Qualcomm sensor HAL
 * (sensors.msm8998.so, which talks to the SLPI through sensors.qcom/libsensor1) and publishes it
 * on the abstract socket "sensorbridge" for the KitKat HAL sensors.taimen (see sensorbridge.h).
 * KitKat (API 19) only knows types 1 to 20 and no wake-up variants: one sensor is kept per
 * type, the non-wake-up one preferably. */
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
#include "../sensorbridge.h"

/* ---- Android 11 sensor HAL structures (hardware/sensors.h, LP64) ---- */
struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
};
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *device);
};
struct sensor_t {
    const char *name;
    const char *vendor;
    int version;
    int handle;
    int type;
    float maxRange;
    float resolution;
    float power;
    int32_t minDelay;
    uint32_t fifoReservedEventCount;
    uint32_t fifoMaxEventCount;
    const char *stringType;
    const char *requiredPermission;
    int64_t maxDelay;
    uint64_t flags;
    void *reserved[2];
};
struct sensors_module_t {
    struct hw_module_t common;
    int (*get_sensors_list)(struct sensors_module_t *module, struct sensor_t const **list);
    int (*set_operation_mode)(unsigned int mode);
};
typedef struct { uint8_t raw[SB_EVENT_SIZE]; } sensors_event_t;
struct sensors_poll_device_1 {
    struct hw_device_t common;
    int (*activate)(struct sensors_poll_device_1 *dev, int handle, int enabled);
    int (*setDelay)(struct sensors_poll_device_1 *dev, int handle, int64_t period_ns);
    int (*poll)(struct sensors_poll_device_1 *dev, sensors_event_t *data, int count);
    int (*batch)(struct sensors_poll_device_1 *dev, int handle, int flags, int64_t period, int64_t timeout);
    int (*flush)(struct sensors_poll_device_1 *dev, int handle);
};
#define SENSOR_FLAG_WAKE_UP 1u
#define HAL_DEVICE_VERSION_1_1 0x0101
/* ---------------------------------------------------------------------------------------- */

static struct sensors_poll_device_1 *s_dev;
static struct sb_sensor s_list[64];
static int s_count;
static int s_active[64];
static int s_client = -1;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

#define LOG(...) do { fprintf(stderr, "sensorbridge: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static int exported(int handle)
{
    for (int i = 0; i < s_count; i++)
        if (s_list[i].handle == handle)
            return i;
    return -1;
}

/* preference: non-wake-up; for steps, the hardware pedometer of the IMU (LSM6DSM), because
 * Qualcomm's (SAM algorithm) goes through a QMI service that cannot be reached here */
static int score(const struct sensor_t *s)
{
    int v = (s->flags & SENSOR_FLAG_WAKE_UP) ? 0 : 2;
    if ((s->type == 18 || s->type == 19) && s->name && strstr(s->name, "LSM6DSM"))
        v += 1;
    return v;
}

static int load(void)
{
    void *so = dlopen("/vendor/lib64/sensors.msm8998.so", RTLD_NOW);
    if (!so) {
        LOG("dlopen: %s", dlerror());
        return -1;
    }
    struct sensors_module_t *m = dlsym(so, "HMI");
    if (!m) {
        LOG("no HMI");
        return -1;
    }
    m->common.dso = so;
    if (m->common.methods->open(&m->common, "poll", (struct hw_device_t **)&s_dev) || !s_dev) {
        LOG("cannot open the HAL");
        return -1;
    }
    const struct sensor_t *list;
    int n = m->get_sensors_list(m, &list);
    LOG("%d sensors (HAL version %x)", n, s_dev->common.version);
    /* one sensor per KitKat type; a non-wake-up one replaces a wake-up one */
    int chosen[21];
    memset(chosen, -1, sizeof(chosen));
    for (int i = 0; i < n; i++) {
        const struct sensor_t *s = &list[i];
        LOG("  [%d] type %d%s %s (%s)", s->handle, s->type,
            (s->flags & SENSOR_FLAG_WAKE_UP) ? " wake-up" : "", s->name, s->stringType ? s->stringType : "");
        if (s->type < 1 || s->type > 20 || s->type == 7 /* TEMPERATURE, deprecated */)
            continue;
        int c = chosen[s->type];
        if (c < 0 || score(s) > score(&list[c]))
            chosen[s->type] = i;
    }
    for (int t = 1; t <= 20; t++) {
        if (chosen[t] < 0)
            continue;
        const struct sensor_t *s = &list[chosen[t]];
        struct sb_sensor *o = &s_list[s_count++];
        memset(o, 0, sizeof(*o));
        snprintf(o->name, sizeof(o->name), "%s", s->name);
        snprintf(o->vendor, sizeof(o->vendor), "%s", s->vendor);
        o->version = s->version;
        o->handle = s->handle;
        o->type = s->type;
        o->maxRange = s->maxRange;
        o->resolution = s->resolution;
        o->power = s->power;
        o->minDelay = s->minDelay;
        o->fifoReservedEventCount = s->fifoReservedEventCount;
        o->fifoMaxEventCount = s->fifoMaxEventCount;
        LOG("exported: [%d] type %d %s", o->handle, o->type, o->name);
    }
    return 0;
}

/* statistics ("sensorbridge stats"): events received from the HAL per sensor, every 5 s */
static int s_stats;
static int s_raw_count[128], s_sent_count[128];
static time_t s_last_stats;

static void stats(void)
{
    time_t now = time(NULL);
    if (!s_stats || now - s_last_stats < 5)
        return;
    s_last_stats = now;
    char line[1024];
    int off = 0;
    for (int h = 0; h < 128; h++)
        if (s_raw_count[h])
            off += snprintf(line + off, sizeof(line) - off, " %d:%d/%d", h, s_raw_count[h], s_sent_count[h]);
    LOG("stats (sensor:received/sent)%s", off ? line : " no events");
}

static void *poll_thread(void *arg)
{
    sensors_event_t ev[64];
    for (;;) {
        int n = s_dev->poll(s_dev, ev, 64);
        for (int i = 0; s_stats && i < n; i++) {
            int32_t sensor, type;
            memcpy(&sensor, ev[i].raw + 4, 4);
            memcpy(&type, ev[i].raw + 8, 4);
            if (sensor >= 0 && sensor < 128)
                s_raw_count[sensor]++;
            if (type != 0 && sensor >= 0 && sensor < 128 && exported(sensor) >= 0 && type != s_list[exported(sensor)].type)
                LOG("event filtered out: sensor %d type %d", sensor, type);
        }
        stats();
        if (n <= 0) {
            if (n < 0)
                usleep(10000);
            continue;
        }
        pthread_mutex_lock(&s_lock);
        if (s_client >= 0) {
            for (int i = 0; i < n; i++) {
                int32_t sensor, type, meta_sensor;
                memcpy(&sensor, ev[i].raw + 4, 4);
                memcpy(&type, ev[i].raw + 8, 4);
                memcpy(&meta_sensor, ev[i].raw + 28, 4);  /* meta_data.sensor */
                /* meta (flush complete) or event of the exported type; no "additional info" */
                int idx = exported(type == 0 ? meta_sensor : sensor);
                if (idx < 0 || (type != 0 && type != s_list[idx].type))
                    continue;
                if (sensor >= 0 && sensor < 128)
                    s_sent_count[sensor]++;
                if (send(s_client, ev[i].raw, SB_EVENT_SIZE, MSG_NOSIGNAL) != SB_EVENT_SIZE) {
                    shutdown(s_client, SHUT_RDWR);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
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

static void serve(int fd)
{
    uint32_t n = s_count;
    if (send(fd, &n, 4, MSG_NOSIGNAL) != 4 ||
        send(fd, s_list, sizeof(s_list[0]) * n, MSG_NOSIGNAL) != (ssize_t)(sizeof(s_list[0]) * n))
        return;
    pthread_mutex_lock(&s_lock);
    s_client = fd;
    pthread_mutex_unlock(&s_lock);
    LOG("client connected");

    struct sb_cmd c;
    while (read_full(fd, &c, sizeof(c)) == 0) {
        int i = exported(c.handle);
        if (i < 0)
            continue;
        int r = 0;
        switch (c.op) {
        case SB_ACTIVATE:
            r = s_dev->activate(s_dev, c.handle, c.a != 0);
            s_active[i] = c.a != 0;
            break;
        case SB_BATCH:
            if (s_dev->common.version >= HAL_DEVICE_VERSION_1_1)
                r = s_dev->batch(s_dev, c.handle, 0, c.a, c.b);
            else
                r = s_dev->setDelay(s_dev, c.handle, c.a);
            break;
        case SB_SET_DELAY:
            if (s_dev->common.version >= HAL_DEVICE_VERSION_1_1)
                r = s_dev->batch(s_dev, c.handle, 0, c.a, 0);
            else
                r = s_dev->setDelay(s_dev, c.handle, c.a);
            break;
        case SB_FLUSH:
            if (s_dev->common.version >= HAL_DEVICE_VERSION_1_1)
                r = s_dev->flush(s_dev, c.handle);
            break;
        }
        if (r)
            LOG("op %u sensor %d: %d", c.op, c.handle, r);
    }

    pthread_mutex_lock(&s_lock);
    s_client = -1;
    pthread_mutex_unlock(&s_lock);
    for (int i = 0; i < s_count; i++) {
        if (s_active[i])
            s_dev->activate(s_dev, s_list[i].handle, 0);
        s_active[i] = 0;
    }
    LOG("client disconnected, sensors off");
}

/* sensorbridge test <handle> [period ms]: enables a sensor and prints 3 s of events */
static int test(int handle, int ms)
{
    int r1 = s_dev->batch(s_dev, handle, 0, (int64_t)ms * 1000000, 0);
    int r2 = s_dev->activate(s_dev, handle, 1);
    printf("batch=%d activate=%d\n", r1, r2);
    sensors_event_t ev[64];
    int total = 0;
    for (int loops = 0; loops < 300 && total < 20; loops++) {
        int n = s_dev->poll(s_dev, ev, 64);
        for (int i = 0; i < n; i++) {
            int32_t sensor, type;
            float v[3];
            int64_t ts;
            memcpy(&sensor, ev[i].raw + 4, 4);
            memcpy(&type, ev[i].raw + 8, 4);
            memcpy(&ts, ev[i].raw + 16, 8);
            memcpy(v, ev[i].raw + 24, 12);
            printf("sensor %d type %d t=%lld: %f %f %f\n", sensor, type, (long long)ts, v[0], v[1], v[2]);
            if (sensor == handle)
                total++;
        }
    }
    s_dev->activate(s_dev, handle, 0);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    s_stats = argc > 1 && !strcmp(argv[1], "stats");  /* hwchroot does not pass the environment */
    if (load())
        return 1;
    if (argc > 2 && !strcmp(argv[1], "test"))
        return test(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 100);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path + 1, SB_SOCKET);
    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SB_SOCKET);
    if (bind(srv, (struct sockaddr *)&addr, len) || listen(srv, 2)) {
        LOG("socket: %s", strerror(errno));
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, poll_thread, NULL);
    LOG("listening on @%s", SB_SOCKET);
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0)
            continue;
        serve(fd);  /* one client at a time: system_server */
        close(fd);
    }
}
