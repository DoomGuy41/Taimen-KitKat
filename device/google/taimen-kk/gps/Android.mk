LOCAL_PATH := $(call my-dir)

# KitKat GPS HAL, client of gnssbridge (Android 11 chroot, modem QMI LOC service)
include $(CLEAR_VARS)
LOCAL_MODULE := gps.taimen
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := gps_taimen.c
LOCAL_CFLAGS := -std=gnu99
LOCAL_SHARED_LIBRARIES := liblog libcutils
include $(BUILD_SHARED_LIBRARY)
