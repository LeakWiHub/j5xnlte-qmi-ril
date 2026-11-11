# Include from the device makefile of the j5xnlte tree:   $(call inherit-product, external/j5xnlte-qmi-ril/android/j5xnlte_ril.mk)
PRODUCT_PACKAGES += android.hardware.radio-service.j5xnlte-qmi
# Only when the vendor does not declare the telephony features itself:
# PRODUCT_PACKAGES += j5xnlte-telephony-features
BOARD_VENDOR_SEPOLICY_DIRS += external/j5xnlte-qmi-ril/android/sepolicy
