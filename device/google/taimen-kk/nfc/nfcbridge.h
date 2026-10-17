/* Protocol between nfcbridge (Android 11 chroot, NXP NFC HAL nfc_nci_nxp.so) and the KitKat HAL
 * nfc_nci.taimen. Abstract SOCK_STREAM socket "nfcbridge", one client at a time.
 * Each message: struct nb_hdr, then len bytes.
 *  - client -> daemon: a HAL operation (NB_OPEN... NB_POWER_CYCLE), answered by NB_RET (int32);
 *  - daemon -> client: NB_RET, and at any time NB_EVT (uint8 event, uint8 status) and NB_DATA
 *    (NCI packet received from the controller). When the client disconnects, the daemon closes
 *    the HAL. */
#ifndef NFCBRIDGE_H
#define NFCBRIDGE_H
#include <stdint.h>

#define NB_SOCKET "nfcbridge"

enum {
    NB_OPEN = 1,
    NB_WRITE = 2,
    NB_CORE_INITIALIZED = 3,  /* payload: complete NCI CORE_INIT_RSP message */
    NB_PRE_DISCOVER = 4,
    NB_CLOSE = 5,
    NB_CONTROL_GRANTED = 6,
    NB_POWER_CYCLE = 7,
    NB_RET = 0x40,
    NB_EVT = 0x41,
    NB_DATA = 0x42,
};

struct nb_hdr {
    uint8_t op;
    uint8_t pad;
    uint16_t len;
};
#endif
