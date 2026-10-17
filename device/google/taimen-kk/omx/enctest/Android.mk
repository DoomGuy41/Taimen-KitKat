LOCAL_PATH := $(call my-dir)

# video encoder test (adb shell enctest <mime> <output> [width height frames])
include $(CLEAR_VARS)
LOCAL_MODULE := enctest
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := enctest.cpp
LOCAL_C_INCLUDES := $(TOP)/frameworks/native/include/media/openmax
LOCAL_SHARED_LIBRARIES := libstagefright libstagefright_foundation libmedia libbinder libgui libutils libcutils
include $(BUILD_EXECUTABLE)
