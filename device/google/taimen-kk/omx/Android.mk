# Vendor OMX plugin (Android 11 Venus hardware codecs through libhyb)
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := libstagefrighthw
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := QcomOMXPlugin.cpp
LOCAL_C_INCLUDES := $(TOP)/frameworks/native/include/media/openmax $(TOP)/frameworks/native/include/media/hardware
LOCAL_SHARED_LIBRARIES := libhyb libutils libcutils liblog
include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
