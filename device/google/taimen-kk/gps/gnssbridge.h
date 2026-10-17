/* Protocol between gnssbridge (Android 11 chroot, QMI LOC client of the modem) and the KitKat GPS
 * HAL (gps.taimen). Abstract SOCK_STREAM socket "gnssbridge", one client at a time.
 *  - the client sends struct gb_cmd; when it disconnects, the daemon stops the session;
 *  - the daemon sends struct gb_hdr messages + payload (len bytes).
 * Same layout in 32 and 64 bits (fields aligned by hand). */
#ifndef GNSSBRIDGE_H
#define GNSSBRIDGE_H
#include <stdint.h>

#define GB_SOCKET "gnssbridge"

enum { GB_START = 1, GB_STOP = 2, GB_INJECT_TIME = 3, GB_INJECT_LOCATION = 4,
       GB_INJECT_XTRA = 5 /* arg: size; followed by the XTRA file (predicted orbits) */ };

struct gb_cmd {
    uint32_t op;
    uint32_t arg;    /* START: interval in ms; INJECT_TIME: uncertainty in ms;
                      * INJECT_LOCATION: accuracy in m */
    int64_t a;       /* INJECT_TIME: UTC time in ms; INJECT_LOCATION: latitude in 1e-7 degree */
    int64_t b;       /* INJECT_LOCATION: longitude in 1e-7 degree */
};

enum { GB_LOCATION = 1, GB_SVSTATUS = 2, GB_NMEA = 3, GB_STATUS = 4,
       GB_XTRA_REQUEST = 5 /* the modem asks for predicted orbits: no payload */ };

struct gb_hdr {
    uint16_t type;
    uint16_t len;
};

/* flags: same values as KitKat's GPS_LOCATION_HAS_* */
#define GB_HAS_LAT_LONG 0x01
#define GB_HAS_ALTITUDE 0x02
#define GB_HAS_SPEED 0x04
#define GB_HAS_BEARING 0x08
#define GB_HAS_ACCURACY 0x10

struct gb_location {
    uint32_t flags;
    float speed;     /* m/s */
    float bearing;   /* degrees */
    float accuracy;  /* m */
    double latitude;
    double longitude;
    double altitude; /* m, WGS84 ellipsoid */
    int64_t utc_ms;
};

#define GB_SV_EPHEMERIS 0x01
#define GB_SV_ALMANAC 0x02
#define GB_SV_USED 0x04
#define GB_MAX_SVS 64
#define GB_MAX_XTRA (1024 * 1024)

/* QMI LOC numbering: GPS 1-32, SBAS 33-64, GLONASS 65-96, BeiDou 201-237, Galileo 301-336 */
struct gb_sv {
    int16_t prn;
    uint8_t system;  /* QMI : 1 GPS, 2 Galileo, 3 SBAS, 4 BeiDou, 5 GLONASS */
    uint8_t flags;
    float snr;       /* dB-Hz */
    float elevation;
    float azimuth;
};

/* GB_SVSTATUS: uint32 count, then count x struct gb_sv
 * GB_NMEA: int64 UTC time in ms, then the sentence (no trailing zero)
 * GB_STATUS: uint32, same values as GPS_STATUS_* (1 session begin, 2 session end, 3 engine on,
 * 4 engine off) */
#endif
