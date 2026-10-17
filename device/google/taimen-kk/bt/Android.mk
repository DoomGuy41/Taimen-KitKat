LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libbt-vendor
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := bt_vendor_taimen.c btinit.c
LOCAL_C_INCLUDES := external/bluetooth/bluedroid/hci/include
LOCAL_CFLAGS := -std=gnu99
LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR_SHARED_LIBRARIES)
include $(BUILD_SHARED_LIBRARY)
