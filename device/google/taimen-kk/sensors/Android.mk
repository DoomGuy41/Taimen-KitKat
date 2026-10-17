# KitKat sensors HAL, client of sensorbridge (Android 11 chroot, Qualcomm SLPI sensor HAL)
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := sensors.taimen
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := sensors_taimen.c
LOCAL_CFLAGS := -std=gnu99
LOCAL_SHARED_LIBRARIES := liblog libcutils
include $(BUILD_SHARED_LIBRARY)
