idf_component_register(SRCS "task_led.c" "main.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS ""
                       REQUIRES esp_driver_gpio bsp)
