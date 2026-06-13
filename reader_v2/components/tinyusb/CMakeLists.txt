# TinyUSB component for ESP-IDF (ESP32-S3 device mode)

# In the Espressif fork, the ESP32-S3 uses Synopsys DWC2 USB controller.
set(COMPONENT_SRCDIR "${COMPONENT_DIR}/src")

set(TINYUSB_SRCS
    "${COMPONENT_SRCDIR}/tusb.c"
    "${COMPONENT_SRCDIR}/common/tusb_fifo.c"
    "${COMPONENT_SRCDIR}/device/usbd.c"
    "${COMPONENT_SRCDIR}/device/usbd_control.c"
    "${COMPONENT_SRCDIR}/class/msc/msc_device.c"
    "${COMPONENT_SRCDIR}/portable/synopsys/dwc2/dcd_dwc2.c"
    "${COMPONENT_SRCDIR}/portable/synopsys/dwc2/dwc2_common.c"
)

set(TINYUSB_INCLUDES
    "${COMPONENT_SRCDIR}"
    "${COMPONENT_DIR}/include"
)

idf_component_register(
    SRCS ${TINYUSB_SRCS}
    INCLUDE_DIRS ${TINYUSB_INCLUDES}
    REQUIRES driver esp_hw_support soc freertos hal
)
