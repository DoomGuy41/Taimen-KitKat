# lights HAL from the Nexus 5 (same qpnp sysfs nodes: lcd-backlight, red/green/blue, on_off_ms, rgb_start)
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := lights.c
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_MODULE := lights.taimen
LOCAL_MODULE_TAGS := optional
include $(BUILD_SHARED_LIBRARY)
