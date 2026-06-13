idf_component_register(SRCS "main.c" "bsp_gpio.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS ""
                       REQUIRES esp_driver_gpio)
