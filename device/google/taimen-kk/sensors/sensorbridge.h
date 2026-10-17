/* Protocol between sensorbridge (Android 11 chroot, Qualcomm sensor HAL) and the KitKat sensor
 * HAL (sensors.taimen). Abstract SOCK_STREAM socket "sensorbridge".
 *  - on connection, the daemon sends: uint32 count, then count x struct sb_sensor;
 *  - after that it only sends sensors_event_t (104 bytes, same layout in 32 and 64 bits);
 *  - the client sends struct sb_cmd. On disconnection, the daemon switches all sensors off. */
#ifndef SENSORBRIDGE_H
#define SENSORBRIDGE_H
#include <stdint.h>

#define SB_SOCKET "sensorbridge"
#define SB_EVENT_SIZE 104

struct sb_sensor {
    char name[64];
    char vendor[64];
    int32_t version;
    int32_t handle;
    int32_t type;
    float maxRange;
    float resolution;
    float power;
    int32_t minDelay;
    uint32_t fifoReservedEventCount;
    uint32_t fifoMaxEventCount;
    int32_t reserved;
};

enum { SB_ACTIVATE = 1, SB_BATCH = 2, SB_SET_DELAY = 3, SB_FLUSH = 4 };

struct sb_cmd {
    uint32_t op;
    int32_t handle;
    int64_t a;   /* ACTIVATE: enabled; BATCH, SET_DELAY: period in ns */
    int64_t b;   /* BATCH: maximum latency in ns */
};
#endif
