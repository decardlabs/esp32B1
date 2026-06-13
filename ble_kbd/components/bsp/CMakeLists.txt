idf_component_register(
    SRCS "bsp_i2c.c" "bsp_spi.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_spi esp_driver_gpio
)
