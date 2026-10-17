// Initialization of the WCN3990 Bluetooth controller (UART ttyHS0) for bluedroid.
// Qualcomm EDL protocol (as in drivers/bluetooth/btqca.c): version, TLV rampatch, TLV NVM,
// then HCI reset. Built as a standalone tool (-DBTINIT_MAIN) or inside libbt-vendor.
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "btinit.h"

#define BT_CMD_PWR_CTRL 0xbfad
#define EDL_PATCH_CMD_OPCODE 0xFC00
#define EDL_PATCH_VER_REQ_CMD 0x19
#define EDL_PATCH_TLV_REQ_CMD 0x1E
#define EDL_SET_BAUDRATE_OPCODE 0xFC48
#define HCI_RESET_OPCODE 0x0C03
#define SEG_MAX 243

static void (*s_log)(const char *msg);
static uint8_t nvm_baud = 0x0E;
static speed_t host_oper = B3000000;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log)
        s_log(buf);
    else
        fprintf(stderr, "btinit: %s\n", buf);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int read_full(int fd, uint8_t *buf, int len, int timeout_ms)
{
    int got = 0;
    long long end = now_ms() + timeout_ms;
    while (got < len) {
        int left = (int)(end - now_ms());
        if (left < 0)
            left = 0;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, left) <= 0)
            return got;
        if (left == 0)
            end = now_ms(); // zero timeout: a single attempt
        int n = read(fd, buf + got, len - got);
        if (n <= 0)
            return got;
        got += n;
    }
    return got;
}

// reads a complete HCI event packet (0x04 type, code, len, data). returns the data size.
static int read_event(int fd, uint8_t *code, uint8_t *data, int timeout_ms)
{
    uint8_t h[3];
    long long end = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - now_ms());
        if (left < 0)
            left = 0;
        if (read_full(fd, h, 1, left) != 1)
            return -1;
        if (h[0] == 0x04)
            break;
        // bytes outside a frame (IBS, noise at power-up): ignored
    }
    if (read_full(fd, h + 1, 2, 1000) != 2)
        return -1;
    *code = h[1];
    if (read_full(fd, data, h[2], 1000) != h[2])
        return -1;
    return h[2];
}

static int send_cmd(int fd, uint16_t opcode, const uint8_t *param, int len)
{
    uint8_t pkt[4 + 255];
    pkt[0] = 0x01;
    pkt[1] = opcode & 0xff;
    pkt[2] = opcode >> 8;
    pkt[3] = len;
    memcpy(pkt + 4, param, len);
    int n = write(fd, pkt, 4 + len);
    return n == 4 + len ? 0 : -1;
}

// command + wait for an event (command complete 0x0E or vendor 0xFF)
static int cmd_sync(int fd, uint16_t opcode, const uint8_t *param, int len, uint8_t *rsp, int timeout_ms)
{
    if (send_cmd(fd, opcode, param, len))
        return -1;
    uint8_t code;
    long long end = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - now_ms());
        int n = read_event(fd, &code, rsp, left > 0 ? left : 0);
        if (n < 0)
            return -1;
        if (code == 0x0E || code == 0x0F || (code == 0xFF && n >= 1 && rsp[0] == 0x00))
            return n | (code << 16);
    }
}

// standard HCI command: waits for the Command Complete of this opcode (other events are ignored)
static int cmd_std(int fd, uint16_t opcode, const uint8_t *param, int len, uint8_t *rsp, int timeout_ms)
{
    if (send_cmd(fd, opcode, param, len))
        return -1;
    uint8_t code;
    long long end = now_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - now_ms());
        int n = read_event(fd, &code, rsp, left > 0 ? left : 0);
        if (n < 0)
            return -1;
        if (code == 0x0E && n >= 3 && (rsp[1] | (rsp[2] << 8)) == opcode)
            return n;
    }
}

static void drain(int fd, int ms)
{
    uint8_t code, buf[260];
    while (read_event(fd, &code, buf, ms) >= 0)
        ;
}

static int set_tty(int fd, speed_t speed, int flow)
{
    struct termios t;
    if (tcgetattr(fd, &t))
        return -1;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    if (flow)
        t.c_cflag |= CRTSCTS;
    else
        t.c_cflag &= ~CRTSCTS;
    cfsetospeed(&t, speed);
    cfsetispeed(&t, speed);
    return tcsetattr(fd, TCSANOW, &t);
}

int btinit_power(int on)
{
    int fd = open("/dev/btpower", O_RDWR);
    if (fd < 0) {
        logf_("open /dev/btpower: %s", strerror(errno));
        return -1;
    }
    int ret = ioctl(fd, BT_CMD_PWR_CTRL, (unsigned long)on);
    close(fd);
    if (ret)
        logf_("btpower %d: %s", on, strerror(errno));
    return ret;
}

static uint8_t *load_file(const char *path, int *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        logf_("open %s: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    fstat(fd, &st);
    uint8_t *buf = malloc(st.st_size);
    *len = read(fd, buf, st.st_size);
    close(fd);
    return buf;
}

static int download_tlv(int fd, const char *path)
{
    int len;
    uint8_t *buf = load_file(path, &len);
    if (!buf)
        return -1;
    // TLV header: type (1), length (3), then for a rampatch: ... download mode
    uint8_t type = buf[0];
    if (type == 4) {
        // NVM: tag 17 (HCI transport) sets the UART speed applied right after the download.
        // 0x11 (3.2 Mbit/s) does not exist in termios: force 3 Mbit/s (0x0E)
        static const uint8_t pat[12] = { 0x11, 0x00, 0x09, 0x00 };
        for (int i = 4; i + 21 <= len; i++) {
            if (!memcmp(buf + i, pat, 12)) {
                logf_("NVM: speed 0x%02x -> 0x%02x, in-band sleep (IBS) %s -> disabled",
                      buf[i + 14], nvm_baud, (buf[i + 12] & 0x80) ? "enabled" : "already off");
                buf[i + 14] = nvm_baud;
                // bit 7 of the first byte: IBS (0xFD/0xFE/0xFC outside frames), unknown to bluedroid's H4
                buf[i + 12] &= ~0x80;
                break;
            }
        }
    }
    int mode = (type == 1 && len > 12) ? buf[12] : 0; // tlv_patch->download_mode (0 = wait for each segment)
    logf_("%s: type %u, %d bytes, mode %d", path, type, len, mode);
    uint8_t cmd[2 + SEG_MAX], rsp[260];
    int off = 0, segs = 0;
    long long t0 = now_ms();
    while (off < len) {
        int seg = len - off > SEG_MAX ? SEG_MAX : len - off;
        cmd[0] = EDL_PATCH_TLV_REQ_CMD;
        cmd[1] = seg;
        memcpy(cmd + 2, buf + off, seg);
        int last = off + seg >= len;
        if (mode != 0 && !last) {
            // "no vendor event" mode: sent directly, draining any Command Complete events on the
            // way so they do not pile up
            if (send_cmd(fd, EDL_PATCH_CMD_OPCODE, cmd, seg + 2))
                goto fail;
            uint8_t code;
            while (read_event(fd, &code, rsp, 0) >= 0)
                ;
        } else {
            int r = cmd_sync(fd, EDL_PATCH_CMD_OPCODE, cmd, seg + 2, rsp, 3000);
            if (r < 0) {
                logf_("no answer to segment %d (offset %d)", segs, off);
                goto fail;
            }
        }
        off += seg;
        segs++;
    }
    logf_("%s sent: %d segments in %lld ms", path, segs, now_ms() - t0);
    free(buf);
    return 0;
fail:
    free(buf);
    return -1;
}

// power pulses of the WCN3990: a circuit on the TX line decodes a byte sent at a given speed
// (0xC0 at 2400 baud = off, 0xFC at 115200 = on), hardware flow control off.
// Wifi shares the same rails: switching btpower off is not enough to restart Bluetooth.
static void power_pulse(int fd, int on)
{
    uint8_t b = on ? 0xFC : 0xC0;
    set_tty(fd, on ? B115200 : B2400, 0);
    tcflush(fd, TCIOFLUSH);
    write(fd, &b, 1);
    tcdrain(fd);
    usleep(on ? 100000 : 10000);
}

int btinit_open(const char *tty, const char *fwdir, void (*log)(const char *))
{
    s_log = log;
    char path[256];
    if (btinit_power(1))
        return -1;
    usleep(50000);
    int fd = open(tty, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        logf_("open %s: %s", tty, strerror(errno));
        return -1;
    }
    // msm_serial_hs: the UART clock is only enabled on request (USERIAL_OP_CLK_ON)
    if (ioctl(fd, 0x5441, NULL) < 0)
        logf_("UART: enabling the clock: %s", strerror(errno));
    power_pulse(fd, 0);
    power_pulse(fd, 1);
    // resynchronize the UART after the controller boots (hci_qca reopens the port)
    close(fd);
    fd = open(tty, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        logf_("reopening %s: %s", tty, strerror(errno));
        return -1;
    }
    ioctl(fd, 0x5441, NULL);
    set_tty(fd, B115200, 1);
    usleep(20000);
    tcflush(fd, TCIOFLUSH);

    uint8_t rsp[260], p = EDL_PATCH_VER_REQ_CMD;
    int r = cmd_sync(fd, EDL_PATCH_CMD_OPCODE, &p, 1, rsp, 2000);
    if (r < 0) {
        logf_("no answer to the version request");
        goto fail;
    }
    {
        char hex[200];
        int n = r & 0xffff, k = 0;
        for (int i = 0; i < n && k < (int)sizeof(hex) - 3; i++)
            k += snprintf(hex + k, sizeof(hex) - k, "%02x", rsp[i]);
        logf_("version (event 0x%02x): %s", r >> 16, hex);
    }

    if (host_oper != B115200) {
        // switch to the operating speed before the download (like hci_qca): the command is
        // sent without waiting, then the host follows, and the confirmation event is dropped
        uint8_t b = nvm_baud;
        set_tty(fd, B115200, 0);
        send_cmd(fd, EDL_SET_BAUDRATE_OPCODE, &b, 1);
        tcdrain(fd);
        usleep(20000);
        set_tty(fd, host_oper, 1);
        drain(fd, 300);
        p = EDL_PATCH_VER_REQ_CMD;
        if (cmd_sync(fd, EDL_PATCH_CMD_OPCODE, &p, 1, rsp, 2000) < 0) {
            logf_("no answer after the speed change");
            goto fail;
        }
        logf_("operating speed OK");
    }

    snprintf(path, sizeof(path), "%s/crbtfw21.tlv", fwdir);
    if (download_tlv(fd, path))
        goto fail;
    snprintf(path, sizeof(path), "%s/crnv21.bin", fwdir);
    if (download_tlv(fd, path))
        goto fail;

    drain(fd, 100);
    r = cmd_std(fd, HCI_RESET_OPCODE, NULL, 0, rsp, 3000);
    if (r < 4) {
        logf_("no answer to HCI reset");
        goto fail;
    }
        logf_("HCI reset: status %u", rsp[3]);
    return fd;
fail:
    close(fd);
    btinit_power(0);
    return -1;
}

// vendor/standard command waiting for a response event (for libbt-vendor)
int btinit_send_cmd_sync(int fd, uint16_t opcode, const uint8_t *param, int len)
{
    uint8_t rsp[260];
    return cmd_sync(fd, opcode, param, len, rsp, 1000);
}

// switches Bluetooth alone off: "off" pulse (wifi shares the power supplies, so btpower stays
// on)
void btinit_shutdown(int fd)
{
    power_pulse(fd, 0);
    close(fd);
}

#ifdef BTINIT_MAIN
int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[3], "115200")) {
        nvm_baud = 0x00;
        host_oper = B115200;
    }
    int fd = btinit_open(argc > 1 ? argv[1] : "/dev/ttyHS0",
                         argc > 2 ? argv[2] : "/system/vendor/firmware", NULL);
    if (fd < 0)
        return 1;
    // read the BD address (Read BD_ADDR 0x1009) to check that the controller answers HCI
    uint8_t rsp[260];
    int r = cmd_std(fd, 0x1009, NULL, 0, rsp, 2000);
    if (r >= 10)
        printf("BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n", rsp[9], rsp[8], rsp[7], rsp[6], rsp[5], rsp[4]);
    close(fd);
    btinit_power(0);
    return 0;
}
#endif
