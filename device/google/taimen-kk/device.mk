LOCAL_PATH := device/google/taimen-kk
# Proprietary files (from the factory image, scripts/extract-blobs.sh) and programs built with the
# NDK (scripts/build-native.sh)
TAIMEN_BLOBS := vendor/google/taimen

PRODUCT_COPY_FILES += \
    $(TAIMEN_BLOBS)/kernel:kernel \
    $(LOCAL_PATH)/init.taimen.rc:root/init.taimen.rc \
    $(LOCAL_PATH)/init.taimen.usb.rc:root/init.taimen.usb.rc \
    $(LOCAL_PATH)/init.taimen.usb.sh:root/init.taimen.usb.sh \
    $(TAIMEN_BLOBS)/bin/busybox:root/sbin/busybox \
    $(LOCAL_PATH)/blackbox.sh:root/blackbox.sh \
    $(LOCAL_PATH)/rtc-restore.sh:root/sbin/rtc-restore.sh \
    $(LOCAL_PATH)/thermal-compat.sh:root/sbin/thermal-compat.sh \
    $(LOCAL_PATH)/v11-sensors.sh:root/sbin/v11-sensors.sh \
    $(LOCAL_PATH)/fstab.taimen:root/fstab.taimen \
    $(LOCAL_PATH)/ueventd.taimen.rc:root/ueventd.taimen.rc \
    frameworks/native/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:system/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:system/etc/permissions/android.hardware.usb.accessory.xml

# Stock kernel modules and firmware (factory image RP1A.201005.004.A1, same vermagic)
PRODUCT_COPY_FILES += \
    $(foreach m,touch_core_base ftm4 sw49408 lge_battery wlan,$(TAIMEN_BLOBS)/modules/$(m).ko:system/lib/modules/$(m).ko) \
    $(foreach f,$(shell cd $(TAIMEN_BLOBS)/firmware && find . -type f -printf '%P\n'),$(TAIMEN_BLOBS)/firmware/$(f):system/vendor/firmware/$(f))

PRODUCT_PACKAGES += \
    taimen_fw_links \
    lights.taimen \
    audio.primary.taimen \
    gralloc.default \
    libGLES_android

PRODUCT_COPY_FILES += $(LOCAL_PATH)/kmsglog.sh:root/sbin/kmsglog.sh

# default wallpaper: downloaded by scripts/fetch-wallpaper.sh (AOSP's otherwise)
DEVICE_PACKAGE_OVERLAYS := $(LOCAL_PATH)/overlay $(wildcard $(TAIMEN_BLOBS)/overlay)

PRODUCT_AAPT_CONFIG := normal large xlarge hdpi xhdpi xxhdpi xxxhdpi
PRODUCT_AAPT_PREF_CONFIG := xxxhdpi

PRODUCT_PROPERTY_OVERRIDES += \
    ro.crypto.fuse_sdcard=true \
    config.disable_atlas=true \
    ro.zygote.disable_gl_preload=true \
    ro.sf.lcd_density=560 \
    dalvik.vm.heapsize=512m \
    ro.opengles.version=196610

PRODUCT_CHARACTERISTICS := nosdcard

# Audio: tinyalsa HAL driving the TAS2557 amplifiers, minimal policy (speaker)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/audio_policy.conf:system/etc/audio_policy.conf

# Google software codecs (without this file stagefright finds no decoder at all: ogg, mp3, aac...)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/media_codecs.xml:system/etc/media_codecs.xml \
    $(LOCAL_PATH)/media_profiles.xml:system/etc/media_profiles.xml

# Modem and wifi: Android 11 Qualcomm daemons in a chroot (/system/v11), logdw for their logs
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/v11.sh:root/sbin/v11.sh \
    $(TAIMEN_BLOBS)/bin/logdw:root/sbin/logdw \
    $(LOCAL_PATH)/wifi/wpa_supplicant.conf:system/etc/wifi/wpa_supplicant.conf \
    $(LOCAL_PATH)/wifi/wpa_supplicant_overlay.conf:system/etc/wifi/wpa_supplicant_overlay.conf \
    frameworks/native/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml

PRODUCT_PACKAGES += \
    taimen_v11_chroot \
    wpa_supplicant \
    dhcpcd.conf

PRODUCT_PROPERTY_OVERRIDES += \
    wifi.interface=wlan0 \
    wifi.supplicant_scan_interval=15

# Bluetooth: WCN3990 on ttyHS0, custom libbt-vendor (power pulse, rampatch, NVM)
PRODUCT_PACKAGES += \
    libbt-vendor \
    audio.a2dp.default

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:system/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:system/etc/permissions/android.hardware.bluetooth_le.xml

# Cellular: reference RIL (AT commands) on the modem AT port
PRODUCT_PACKAGES += \
    rild \
    libreference-ril

PRODUCT_PROPERTY_OVERRIDES += \
    rild.libpath=/system/lib/libreference-ril.so \
    ro.telephony.default_network=9 \
    telephony.lteOnGsmDevice=1

# GPU: the real Android 11 Adreno driver (EGL 1.5, OpenGL ES 3.2) and QTI gralloc1, loaded into
# every KitKat process by libhyb (root /system/a11); KitKat EGL/GLES and gralloc shims
A11_DIR := $(TAIMEN_BLOBS)/a11
PRODUCT_COPY_FILES += \
    $(foreach f,$(shell cd $(A11_DIR) && find . -type f -printf '%P\n'),$(A11_DIR)/$(f):system/a11/$(f))
# zero-copy presentation: fbpresentd (64-bit, MDSS atomic commit; source in fbpresentd/)
PRODUCT_COPY_FILES += $(TAIMEN_BLOBS)/bin/fbpresentd:system/bin/fbpresentd
PRODUCT_PACKAGES += \
    libhyb \
    gralloc.taimen \
    libEGL_adreno \
    libGLESv1_CM_adreno \
    libGLESv2_adreno \
    libstagefrighthw

# GPS: modem GNSS engine (QMI LOC) through gnssbridge in the Android 11 chroot, gps.taimen HAL
PRODUCT_PACKAGES += gps.taimen
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/v11-gnss.sh:root/sbin/v11-gnss.sh \
    $(LOCAL_PATH)/gps/gps.conf:system/etc/gps.conf \
    frameworks/native/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml

# Camera: the Android 11 Qualcomm HAL (camera.msm8998, mm-camera, Easel) loaded by libhyb into
# mediaserver, translated for KitKat by camera.taimen (module 2.0, HAL3 3.0). ZSL and HDR+ off.
PRODUCT_PACKAGES += camera.taimen
PRODUCT_COPY_FILES += \
    $(foreach f,$(shell cd $(TAIMEN_BLOBS)/camera && ls),$(TAIMEN_BLOBS)/camera/$(f):system/vendor/etc/camera/$(f)) \
    $(foreach f,camera camera.front camera.autofocus camera.flash-autofocus,frameworks/native/data/etc/android.hardware.$(f).xml:system/etc/permissions/android.hardware.$(f).xml)
PRODUCT_PROPERTY_OVERRIDES += \
    camera.disable_zsl_mode=1 \
    persist.camera.hdrplus.disable=1

# NFC: NXP PN553 driven by the Android 11 NXP HAL in the chroot (nfcbridge, which translates
# NCI 2.0 to 1.0), KitKat NCI stack (NfcNci) through the nfc_nci.taimen HAL
PRODUCT_PACKAGES += nfc_nci.taimen NfcNci Tag libnfc-nci libnfc_nci_jni com.android.nfc_extras
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/v11-nfc.sh:root/sbin/v11-nfc.sh \
    $(LOCAL_PATH)/nfc/libnfc-brcm.conf:system/etc/libnfc-brcm.conf \
    frameworks/native/data/etc/android.hardware.nfc.xml:system/etc/permissions/android.hardware.nfc.xml \
    frameworks/base/nfc-extras/com.android.nfc_extras.xml:system/etc/permissions/com.android.nfc_extras.xml

# Stock Pixel boot animation (Android 11): 1440x3040, trimmed frames (trim.txt) and background
# colour, supported by the patched KitKat BootAnimation
PRODUCT_COPY_FILES += $(TAIMEN_BLOBS)/bootanimation.zip:system/media/bootanimation.zip

# Sensors: SLPI through sensors.qcom + the Android 11 Qualcomm HAL (chroot), sensorbridge
PRODUCT_PACKAGES += sensors.taimen
PRODUCT_COPY_FILES += \
    $(foreach f,accelerometer barometer compass gyroscope light proximity stepcounter stepdetector,frameworks/native/data/etc/android.hardware.sensor.$(f).xml:system/etc/permissions/android.hardware.sensor.$(f).xml)

# ---------------------------------------------------------------------------------------------
# user variant (release build): USB in MTP mode, adb only when enabled in Developer options (and
# authorized), Google services when vendor/google/gapps-kk exists. Other variants keep the
# bring-up tools: diagnostics app, test programs, adb without authorization.
ifeq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += persist.sys.usb.config=mtp
$(call inherit-product-if-exists, vendor/google/gapps-kk/gapps-kk.mk)
else
PRODUCT_PACKAGES += \
    TaimenDiag \
    tinyplay tinymix tinycap \
    hybtest camtest glbench enctest
PRODUCT_PROPERTY_OVERRIDES += ro.adb.secure=0
endif
