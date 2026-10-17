/*
 * libbt-vendor for the WCN3990 of the Pixel 2 XL (UART ttyHS0), for KitKat's bluedroid.
 * Opening the UART does the whole initialization (power pulse, 3 Mbit/s, rampatch + NVM,
 * HCI reset, BD address): the firmware configuration step is then immediate.
 */
#define LOG_TAG "bt_vendor_taimen"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <utils/Log.h>

#include "bt_vendor_lib.h"
#include "btinit.h"

#define BT_TTY "/dev/ttyHS0"
#define BT_FW_DIR "/system/vendor/firmware"

int btinit_send_cmd_sync(int fd, uint16_t opcode, const uint8_t *param, int len);

static const bt_vendor_callbacks_t *s_cb;
static uint8_t s_bdaddr[6];
static int s_fd = -1;

static void log_cb(const char *msg)
{
    ALOGI("%s", msg);
}

static int vnd_init(const bt_vendor_callbacks_t *cb, unsigned char *bdaddr)
{
    s_cb = cb;
    if (bdaddr)
        memcpy(s_bdaddr, bdaddr, 6);
    ALOGI("init, address %02x:%02x:%02x:%02x:%02x:%02x", s_bdaddr[0], s_bdaddr[1], s_bdaddr[2],
          s_bdaddr[3], s_bdaddr[4], s_bdaddr[5]);
    return 0;
}

static int vnd_op(bt_vendor_opcode_t opcode, void *param)
{
    switch (opcode) {
    case BT_VND_OP_POWER_CTRL:
        // the actual power is handled when the UART is opened and closed
        return 0;
    case BT_VND_OP_USERIAL_OPEN: {
        int *fds = (int *)param;
        s_fd = btinit_open(BT_TTY, BT_FW_DIR, log_cb);
        if (s_fd < 0)
            return -1;
        // BD address given by bluedroid (most significant byte first): the Qualcomm command
        // 0xFC14 expects that same order (Linux btqca.c: baswap before EDL_WRITE_BD_ADDR)
        if (btinit_send_cmd_sync(s_fd, 0xFC14, s_bdaddr, 6) < 0)
            ALOGW("no answer when writing the BD address");
        for (int i = 0; i < CH_MAX; i++)
            fds[i] = s_fd;
        return 1;
    }
    case BT_VND_OP_USERIAL_CLOSE:
        if (s_fd >= 0) {
            btinit_shutdown(s_fd);
            s_fd = -1;
        }
        return 0;
    case BT_VND_OP_FW_CFG:
        s_cb->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    case BT_VND_OP_SCO_CFG:
        s_cb->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
        *(uint32_t *)param = 3000;
        return 0;
    case BT_VND_OP_LPM_SET_MODE:
        s_cb->lpm_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    case BT_VND_OP_LPM_WAKE_SET_STATE:
        return 0;
    case BT_VND_OP_EPILOG:
        s_cb->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
        return 0;
    }
    return -1;
}

static void vnd_cleanup(void)
{
    if (s_fd >= 0) {
        btinit_shutdown(s_fd);
        s_fd = -1;
    }
    s_cb = NULL;
}

const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
    sizeof(bt_vendor_interface_t),
    vnd_init,
    vnd_op,
    vnd_cleanup,
};
