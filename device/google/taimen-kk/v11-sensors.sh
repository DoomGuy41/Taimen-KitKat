# Run by /sbin/busybox sh (service v11-sensors, once the chroot is ready)
# Boots the SLPI (sensor DSP), then sensors.qcom (QMI client of the SLPI) and sensorbridge,
# which serves the Qualcomm sensor HAL to the KitKat HAL sensors.taimen.
B=/sbin/busybox
R=/system/v11/root
log() { echo "v11-sensors: $*" > /dev/kmsg; }

mkdir -p /data/v11data/vendor/sensors
chown 1000:1000 /data/v11data/vendor/sensors
chown 0:1000 /dev/sensors
chmod 660 /dev/sensors

echo 1 > /sys/kernel/boot_slpi/boot
i=0
while [ $i -lt 40 ]; do
    for s in /sys/bus/msm_subsys/devices/*; do
        [ "$(cat $s/name)" = slpi ] && [ "$(cat $s/state)" = ONLINE ] && i=99
    done
    [ $i -eq 99 ] && break
    $B sleep 0.25; i=$((i+1))
done
log "SLPI $([ $i -eq 99 ] && echo online || echo offline)"

/system/v11/hwchroot $R /vendor/bin/sensors.qcom &
$B sleep 2
exec /system/v11/hwchroot $R /vendor/bin/sensorbridge 2>/dev/kmsg
