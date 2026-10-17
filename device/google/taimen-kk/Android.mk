LOCAL_PATH := $(call my-dir)

# TAS2557 amplifier calibration: specific to each phone, stored in /persist
include $(CLEAR_VARS)
LOCAL_MODULE := taimen_fw_links
LOCAL_MODULE_TAGS := optional
LOCAL_POST_INSTALL_CMD := mkdir -p $(TARGET_OUT)/vendor/firmware && \
    ln -sf /persist/audio/tas2557_cal.bin $(TARGET_OUT)/vendor/firmware/tas2557_cal.bin
include $(BUILD_PHONY_PACKAGE)
# Android 11 chroot of the Qualcomm daemons: copied as is (rfs/ holds absolute symbolic links)
include $(CLEAR_VARS)
LOCAL_MODULE := taimen_v11_chroot
LOCAL_MODULE_TAGS := optional
LOCAL_ADDITIONAL_DEPENDENCIES := $(shell find vendor/google/taimen/v11 -type f)
LOCAL_POST_INSTALL_CMD := rm -rf $(TARGET_OUT)/v11 && cp -a vendor/google/taimen/v11 $(TARGET_OUT)/v11
include $(BUILD_PHONY_PACKAGE)

include $(call all-makefiles-under,$(LOCAL_PATH))
