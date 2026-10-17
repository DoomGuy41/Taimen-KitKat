# Run by /sbin/busybox sh (service v11-nfc, once the chroot is ready)
# nfcbridge: the Android 11 NXP NFC HAL (PN553 controller, NCI 2.0 translated to 1.0 for the
# KitKat stack), served to the KitKat HAL nfc_nci.taimen; logs in kmsg.
mkdir -p /data/v11data/vendor/nfc/param
exec /system/v11/hwchroot /system/v11/root /vendor/bin/nfcbridge 2>/dev/kmsg
