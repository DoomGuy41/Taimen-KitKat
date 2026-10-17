# Pixel 2 XL (taimen): 32-bit KitKat userspace on the stock 64-bit kernel
TARGET_ARCH := arm
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_CPU_VARIANT := cortex-a15
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_CPU_SMP := true
TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true
TARGET_NO_RECOVERY := true
TARGET_BOARD_PLATFORM := msm8998
TARGET_BOOTLOADER_BOARD_NAME := taimen

# Stock 4.4 kernel (skip_initramfs renamed want_initramfs by scripts/patch-kernel.py), lz4 + DTB
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
BOARD_MKBOOTIMG_ARGS := --ramdisk_offset 0x01000000 --tags_offset 0x00000100
BOARD_KERNEL_CMDLINE := console=ttyMSM0,115200,n8 earlycon=msm_serial_dm,0xc1b0000 androidboot.hardware=taimen androidboot.console=ttyMSM0 lpm_levels.sleep_disabled=1 user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3 service_locator.enable=1 androidboot.baseband=msm swiotlb=2048 firmware_class.path=/vendor/firmware loop.max_part=7 raid=noautodetect usbcore.autosuspend=7 androidboot.dtbo_idx=12 androidboot.selinux=disabled

TARGET_USERIMAGES_USE_EXT4 := true
BOARD_BOOTIMAGE_PARTITION_SIZE := 41943040
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 1610612736
BOARD_USERDATAIMAGE_PARTITION_SIZE := 54108598272
BOARD_FLASH_BLOCK_SIZE := 4096

# GPU: Android 11 Adreno driver loaded by libhyb (libEGL_adreno), libagl as fallback
BOARD_EGL_CFG := device/google/taimen-kk/egl.cfg
USE_OPENGL_RENDERER := true
TARGET_DISABLE_TRIPLE_BUFFERING := true

BOARD_HAVE_BLUETOOTH := true

# dexpreopt at build time: avoids a first boot that dexopts everything on the phone
WITH_DEXPREOPT := true

# Wifi: qca_cld driver (stock wlan.ko) over nl80211, firmware in the modem
WPA_SUPPLICANT_VERSION      := VER_0_8_X
BOARD_WLAN_DEVICE           := qcwcn
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
BOARD_HOSTAPD_DRIVER        := NL80211
BOARD_HOSTAPD_PRIVATE_LIB   := lib_driver_cmd_$(BOARD_WLAN_DEVICE)
WIFI_DRIVER_FW_PATH_PARAM   := "/sys/module/wlan/parameters/fwpath"
WIFI_DRIVER_FW_PATH_STA     := "sta"
WIFI_DRIVER_FW_PATH_AP      := "ap"
WIFI_DRIVER_FW_PATH_P2P     := "p2p"
TARGET_SYSTEM_PROP := device/google/taimen-kk/system.prop

# user variant signed with private release keys (see aosp_taimen_kk.mk): tagged release-keys
TAIMEN_KEYS ?= $(HOME)/.android-certs/taimen-kk
ifeq ($(TARGET_BUILD_VARIANT),user)
ifneq ($(wildcard $(TAIMEN_KEYS)/releasekey.pk8),)
TARGET_SIGNED_WITH_RELEASE_KEYS := true
endif
endif
