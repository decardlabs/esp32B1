idf_component_register(SRCS "task_uart.c" "task_exti_prco.c" "task_led.c" "main.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "include" ""    
                       REQUIRES esp_driver_gpio bsp)
