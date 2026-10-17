LOCAL_PATH := $(call my-dir)

# on-screen GPU benchmark (adb shell glbench [seconds])
include $(CLEAR_VARS)
LOCAL_MODULE := glbench
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := glbench.cpp
LOCAL_SHARED_LIBRARIES := libEGL libGLESv2 libgui libui libutils libbinder libcutils
include $(BUILD_EXECUTABLE)
