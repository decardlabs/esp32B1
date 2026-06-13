idf_component_register(
    SRCS "main.c" "lcd_st7796.c" "sd_card.c" "tusb_msc.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd sdmmc esp_driver_sdspi tinyusb usb
)
