# Full APN list (before inheriting: the first copy of a destination wins)
PRODUCT_COPY_FILES := device/sample/etc/apns-full-conf.xml:system/etc/apns-conf.xml
# no data roaming by default (full_base_telephony enables it; ro.*: the first value wins)
PRODUCT_PROPERTY_OVERRIDES := ro.com.android.dataroaming=false

# Apps of a complete KitKat AOSP: telephony (Dialer + InCallUI, Mms, VoiceDialer) like
# aosp_hammerhead, Launcher3 (replaces Launcher2), SIM toolkit and emergency alerts
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)
PRODUCT_PACKAGES += \
    Launcher3 \
    Stk \
    CellBroadcastReceiver
$(call inherit-product, device/google/taimen-kk/device.mk)
# Required: without a sized Dalvik heap, system_server runs out of memory while scanning the APKs
$(call inherit-product, frameworks/native/build/phone-xhdpi-2048-dalvik-heap.mk)
PRODUCT_NAME := aosp_taimen_kk
PRODUCT_DEVICE := taimen-kk
PRODUCT_BRAND := google
PRODUCT_MODEL := Pixel 2 XL
PRODUCT_MANUFACTURER := Google

# user builds are signed with your own release keys when they exist (scripts/make-keys.sh), with
# the public AOSP test keys otherwise. TAIMEN_KEYS is also read by BoardConfig.mk.
TAIMEN_KEYS ?= $(HOME)/.android-certs/taimen-kk
ifeq ($(TARGET_BUILD_VARIANT),user)
ifneq ($(wildcard $(TAIMEN_KEYS)/releasekey.pk8),)
PRODUCT_DEFAULT_DEV_CERTIFICATE := $(TAIMEN_KEYS)/releasekey
endif
endif
