LOCAL_PATH := $(call my-dir)

# KitKat NFC HAL, client of nfcbridge (Android 11 chroot, NXP HAL)
include $(CLEAR_VARS)
LOCAL_MODULE := nfc_nci.taimen
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := nfc_taimen.c
LOCAL_CFLAGS := -std=gnu99 -Wall -Wno-unused-parameter
LOCAL_SHARED_LIBRARIES := liblog libcutils
include $(BUILD_SHARED_LIBRARY)
