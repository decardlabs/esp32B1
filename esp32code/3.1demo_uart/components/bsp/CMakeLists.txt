idf_component_register(SRCS "bsp_exti.c" "bsp_gpio.c" "bsp_uart.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio esp_timer driver)
