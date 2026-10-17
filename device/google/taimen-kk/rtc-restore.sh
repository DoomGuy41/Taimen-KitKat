#!/sbin/busybox sh
# wall time = RTC (read-only, counts from 1970) + offset saved by system_server
B=/sbin/busybox
o=$($B cat /data/system/time/rtc_offset 2>/dev/null) || exit 0
r=$($B cat /sys/class/rtc/rtc0/since_epoch) || exit 0
$B date -s @$((r + o)) >/dev/null
