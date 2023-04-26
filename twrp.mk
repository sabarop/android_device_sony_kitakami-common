#
# This is for TWRP Recovery
#

COMMON_PATH := device/sony/kitakami-common
TWRP_OUT := recovery/root

TW_MAX_BRIGHTNESS := 255
TARGET_RECOVERY_FSTAB := $(COMMON_PATH)/rootdir/recovery.fstab

PRODUCT_COPY_FILES += \
    $(COMMON_PATH)/rootdir/sbin/pulldecryptfiles.sh:$(TWRP_OUT)/sbin/pulldecryptfiles.sh

PRODUCT_PROPERTY_OVERRIDES += \
    ro.usb.pid_suffix=1CF
