idf_component_register(SRCS "bsp_exti.c" "bsp_gpio.c" "bsp_uart.c" "bsp_gptimer.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio esp_timer esp_driver_gptimer esp_driver_uart)
