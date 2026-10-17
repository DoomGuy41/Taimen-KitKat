// Minimal receiver for the logdw socket of Android 8+ (the daemons of the Android 11 chroot
// write their logs to it), copied to /dev/kmsg for the kernel log / persistent log.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int main(void)
{
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, "/dev/socket/logdw");
    unlink(a.sun_path);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("logdw: bind");
        return 1;
    }
    chmod(a.sun_path, 0222);
    int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    static const char lv[] = "77765433"; // android priority -> kmsg level
    unsigned char buf[5000];
    for (;;) {
        ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
        // header: id (1), tid (2), realtime (8), then priority (1), tag\0, message\0
        if (n < 13 || buf[0] == 2 /* LOG_ID_EVENTS */ || buf[0] == 3 /* LOG_ID_SECURITY */)
            continue;
        buf[n] = 0;
        unsigned prio = buf[11];
        // warnings and errors only: otherwise tftp_server floods the kernel buffer within a
        // few seconds (info logs on every exchange with the modem)
        if (prio < 5 /* ANDROID_LOG_WARN */)
            continue;
        const char *tag = (const char *)buf + 12;
        const char *msg = tag + strlen(tag) + 1;
        if (msg >= (const char *)buf + n)
            msg = "";
        char line[1100];
        int len = snprintf(line, sizeof(line), "<%c>%s: %s", lv[prio & 7], tag, msg);
        if (len > 0)
            write(k, line, len < (int)sizeof(line) ? len : (int)sizeof(line) - 1);
    }
}
