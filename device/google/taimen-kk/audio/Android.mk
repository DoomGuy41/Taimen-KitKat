LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := audio.primary.taimen
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SRC_FILES := audio_hw.c
LOCAL_C_INCLUDES += external/tinyalsa/include
LOCAL_SHARED_LIBRARIES := liblog libcutils libtinyalsa
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)
