#!/sbin/busybox sh
# thermal_zone0..3 belong to the Pixel Visual Core (mnh_*), powered off, and return EIO.
# CPU temperature apps read thermal_zone0 first: bind the SoC tsens sensors over them
# (cpu0, cpu4, cpu5, GPU; values in tenths of a degree).
B=/sbin/busybox
T=/sys/devices/virtual/thermal
z() {
    for i in $($B seq 4 60); do
        [ "$($B cat $T/thermal_zone$i/type 2>/dev/null)" = "$2" ] || continue
        $B mount -o bind $T/thermal_zone$i/temp $T/thermal_zone$1/temp
        $B mount -o bind $T/thermal_zone$i/type $T/thermal_zone$1/type
        return
    done
}
z 0 tsens_tz_sensor1
z 1 tsens_tz_sensor7
z 2 tsens_tz_sensor8
z 3 tsens_tz_sensor12
