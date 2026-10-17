# Persistent log (eng builds): kmsg (kernel + Android logs through liblog) copied to /data/log.
# The log of the previous boot is kept in kmsg.prev.txt (survives crashes).
B=/sbin/busybox
$B mkdir -p /data/log
[ -f /data/log/kmsg.txt ] && $B mv -f /data/log/kmsg.txt /data/log/kmsg.prev.txt
exec $B cat /dev/kmsg > /data/log/kmsg.txt
