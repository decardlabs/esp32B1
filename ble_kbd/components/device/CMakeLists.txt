idf_component_register(
    SRCS "xl9555.c" "lcd_st7796.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp esp_driver_i2c esp_driver_gpio esp_driver_spi esp_lcd
)
