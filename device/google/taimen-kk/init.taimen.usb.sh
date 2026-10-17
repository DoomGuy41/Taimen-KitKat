# Run by /sbin/busybox sh (does not depend on /system)
# Watches the USB gadget:
#  - when adbd restarts (unplug), FunctionFS detaches the UDC: attach it again as soon as the
#    descriptors are written again (ep1 exists);
#  - cable plugged in (VBUS) but the host has not configured the gadget for 15 s (common after a
#    reboot from fastboot): detach/attach the controller, like replugging the cable.
B=/sbin/busybox
U=/config/usb_gadget/g1/UDC
S=/sys/class/udc/a800000.dwc3/state
V=/sys/class/power_supply/usb/present
waiting=0
while true; do
    if [ -e /dev/usb-ffs/adb/ep1 ] && [ -z "$($B cat $U 2>/dev/null)" ]; then
        echo a800000.dwc3 > $U 2>/dev/null && echo "usb-bind: UDC attached" > /dev/kmsg
        waiting=0
    fi
    if [ "$($B cat $V 2>/dev/null)" = 1 ] && [ "$($B cat $S 2>/dev/null)" != configured ] &&
       [ -n "$($B cat $U 2>/dev/null)" ]; then
        waiting=$((waiting+1))
        if [ $waiting -ge 15 ]; then
            echo "usb-bind: no answer from the host for 15 s (state $($B cat $S)), reconnecting" > /dev/kmsg
            echo "" > $U
            $B sleep 1
            echo a800000.dwc3 > $U 2>/dev/null
            waiting=0
        fi
    else
        waiting=0
    fi
    $B sleep 1
done
