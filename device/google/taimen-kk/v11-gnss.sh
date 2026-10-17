# Run by /sbin/busybox sh (service v11-gnss, once the chroot is ready)
# gnssbridge: QMI LOC client of the modem, served to the KitKat HAL gps.taimen; logs in kmsg.
exec /system/v11/hwchroot /system/v11/root /vendor/bin/gnssbridge 2>/dev/kmsg
