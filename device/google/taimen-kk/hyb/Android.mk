# libhyb: loads Android 11 libraries (Adreno driver, QTI gralloc, camera HAL...) into KitKat
# processes; root of the Android 11 files: /system/a11
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := libhyb
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := linker.c hooks.c
LOCAL_CFLAGS := -std=gnu99 -O2 -Wall -Wno-unused-function -Wno-unused-parameter
LOCAL_SHARED_LIBRARIES := libcutils liblog libdl
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := hybtest
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := hybtest.c
LOCAL_CFLAGS := -std=gnu99 -O2
LOCAL_SHARED_LIBRARIES := libhyb liblog
include $(BUILD_EXECUTABLE)
