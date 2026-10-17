# KitKat gralloc on top of the Android 11 QTI gralloc1 (libhyb)
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := gralloc.taimen
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := gralloc.c
LOCAL_CFLAGS := -std=gnu99 -O2 -Wall -Wno-unused-parameter
LOCAL_SHARED_LIBRARIES := libhyb libcutils liblog
include $(BUILD_SHARED_LIBRARY)

