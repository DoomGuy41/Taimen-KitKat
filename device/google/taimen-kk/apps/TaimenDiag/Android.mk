# Diagnostics app (eng builds, plain text): every value of the phone and hardware tests.
# System app (system uid) to drive the flash, LED and backlight sysfs nodes directly.
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE_TAGS := optional
LOCAL_PACKAGE_NAME := TaimenDiag
LOCAL_SRC_FILES := $(call all-java-files-under, src)
LOCAL_CERTIFICATE := platform
LOCAL_PRIVILEGED_MODULE := true
LOCAL_PROGUARD_ENABLED := disabled
include $(BUILD_PACKAGE)
