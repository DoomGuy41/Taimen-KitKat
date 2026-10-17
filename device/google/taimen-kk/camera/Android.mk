LOCAL_PATH := $(call my-dir)

# camtest: exercises the Android 11 Qualcomm camera HAL loaded by libhyb (eng builds)
include $(CLEAR_VARS)
LOCAL_MODULE := camtest
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := camtest.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../hyb
LOCAL_CFLAGS := -std=gnu99 -O2
LOCAL_SHARED_LIBRARIES := libhyb liblog libhardware
include $(BUILD_EXECUTABLE)

# KitKat camera HAL (module 2.0, HAL3 3.0 devices) on top of the Android 11 Qualcomm HAL
include $(CLEAR_VARS)
LOCAL_MODULE := camera.taimen
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := camera_taimen.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../hyb $(TOP)/system/media/camera/include
LOCAL_CFLAGS := -std=gnu99 -O2 -Wall -Wno-unused-parameter
LOCAL_SHARED_LIBRARIES := libhyb libcamera_metadata liblog libcutils
include $(BUILD_SHARED_LIBRARY)
