#!/sbin/busybox sh
# Debug black box (eng builds): copies dmesg + state to system_a at 2 GiB (past the filesystem)
B=/sbin/busybox
DEV=/dev/block/sda7
OFF=524288            # in 4 KiB blocks = 2 GiB
while [ ! -b $DEV ]; do $B sleep 1; done
# Safety check: write ONLY if sda7 really is system_a
$B grep -q PARTNAME=system_a /sys/class/block/sda7/uevent || { echo "blackbox: sda7 is not system_a, giving up" > /dev/kmsg; exit 1; }
echo "blackbox: started" > /dev/kmsg
i=0
while true; do
  {
    echo "=== BLACKBOX passe=$i uptime=$($B cat /proc/uptime)"
    echo "=== PS"; $B ps
    echo "=== MOUNTS"; $B cat /proc/mounts
    echo "=== DEV"; $B ls -la /dev/block 2>&1; $B ls -la /dev/usb-ffs/adb 2>&1; $B ls -la /config/usb_gadget/g1/configs/b.1 2>&1
    echo "=== UDC"; $B cat /config/usb_gadget/g1/UDC 2>&1; $B ls /sys/class/udc 2>&1
    echo "=== DMESG"; $B dmesg
    echo "=== END ==="
  } > /dev/blackbox.txt 2>&1
  $B dd if=/dev/blackbox.txt of=$DEV bs=4096 seek=$OFF conv=notrunc,fsync 2>/dev/null
  i=$((i+1))
  $B sleep 2
done
