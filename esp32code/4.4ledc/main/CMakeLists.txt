idf_component_register(SRCS "task_timer.c" "task_exti_prco.c" "task_led.c" "main.c" "task_uart.c" "task_ledc.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "include" ""    
                       REQUIRES esp_driver_gpio bsp driver)
