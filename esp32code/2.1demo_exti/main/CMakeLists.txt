idf_component_register(SRCS "task_led.c" "main.c" "task_exti.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "include" ""
                       REQUIRES esp_driver_gpio bsp )
