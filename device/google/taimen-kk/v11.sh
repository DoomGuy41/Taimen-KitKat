# Run by /sbin/busybox sh (service v11-setup, once /data is mounted)
# Prepares the Android 11 chroot of the Qualcomm daemons, boots the modem (which hosts the
# wifi firmware), then loads the wifi driver.
B=/sbin/busybox
R=/system/v11/root
log() { echo "v11: $*" > /dev/kmsg; }

# rmt_storage opens /dev/block/bootdevice/by-name/{modemst1,modemst2,fsg,fsc}
mkdir -p /dev/block/bootdevice/by-name
for d in /sys/class/block/sd*; do
    n=$($B grep PARTNAME $d/uevent | $B cut -d= -f2)
    [ -n "$n" ] && $B ln -sf /dev/block/${d##*/} /dev/block/bootdevice/by-name/$n
done

# modem partition (vfat): modem firmware, also /firmware for tftp_server
$B mount -t vfat -o ro,shortname=lower,uid=1000,gid=1000,dmask=227,fmask=337 /dev/block/sde5 $R/vendor/firmware_mnt
$B mount --bind $R/vendor/firmware_mnt $R/firmware
# baseband version (About phone): the one of the radio image, as on the stock ROM
v=$($B sed -n 's/^SSD://p' $R/vendor/firmware_mnt/image/version.cfg 2>/dev/null | $B tr -d '\r\n')
[ -n "$v" ] && setprop gsm.version.baseband "$v"
$B mount --bind /persist $R/mnt/vendor/persist
$B mount --bind /persist $R/persist
mkdir -p /data/v11data/vendor
$B mount --bind /data/v11data $R/data
# IPC router security rules: without them non-root QMI clients wait forever
/system/v11/hwchroot $R /vendor/bin/irsc_util /vendor/etc/sec_config
log "chroot ready (irsc_util=$?)"
setprop sys.v11.ready 1

# the modem only boots once rmt_storage (EFS) and tftp_server are running
i=0
while [ $i -lt 20 ]; do
    [ "$(getprop init.svc.v11-rmt)" = running ] && [ "$(getprop init.svc.v11-tftp)" = running ] && break
    $B sleep 0.5; i=$((i+1))
done

# PIL knows a single firmware path at a time: the modem partition's while the modem boots
echo -n $R/vendor/firmware_mnt/image > /sys/module/firmware_class/parameters/path
exec 3</dev/subsys_modem
echo -n /vendor/firmware > /sys/module/firmware_class/parameters/path
for s in /sys/bus/msm_subsys/devices/*; do
    [ "$(cat $s/name)" = modem ] && log "modem: $(cat $s/state)"
done

# wifi driver: it waits for the modem wlan firmware to show up (pd-mapper + cnss-daemon)
insmod /system/lib/modules/wlan.ko && log "wlan.ko loaded"
chown 1010:1010 /sys/module/wlan/parameters/fwpath  # wifi, for netd's fwreload
i=0
while [ ! -e /sys/class/net/wlan0 ] && [ $i -lt 60 ]; do $B sleep 0.5; i=$((i+1)); done
[ -e /sys/class/net/wlan0 ] && log "wlan0 up" || log "no wlan0 after 30 s"

# keep /dev/subsys_modem open (closing it shuts the modem down)
exec $B sleep 2147483647
