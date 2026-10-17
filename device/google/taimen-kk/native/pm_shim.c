// Replaces libperipheral_client.so in the Android 11 chroot. The original goes through vndbinder
// to pm-service (missing on KitKat) and blocks cnss-daemon forever. Here the modem is already
// kept powered by /sbin/v11.sh: simply answer "online".
#include <stdlib.h>

enum pm_event {
    EVENT_PERIPH_GOING_OFFLINE,
    EVENT_PERIPH_IS_OFFLINE,
    EVENT_PERIPH_GOING_ONLINE,
    EVENT_PERIPH_IS_ONLINE,
};
typedef void (*pm_client_notifier)(void *, enum pm_event);

int pm_client_register(pm_client_notifier cb, void *data, const char *peripheral,
                       const char *client, enum pm_event *state, void **handle)
{
    (void)cb; (void)data; (void)peripheral; (void)client;
    if (state)
        *state = EVENT_PERIPH_IS_ONLINE;
    if (handle)
        *handle = malloc(1);
    return 0;
}
int pm_client_connect(void *h) { (void)h; return 0; }
int pm_client_disconnect(void *h) { (void)h; return 0; }
int pm_client_unregister(void *h) { free(h); return 0; }
int pm_client_event_acknowledge(void *h, enum pm_event e) { (void)h; (void)e; return 0; }
int pm_service_shutdown(void) { return 0; }
int pm_show_peripherals(void) { return 0; }
