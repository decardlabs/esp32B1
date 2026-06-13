idf_component_register(SRCS "bsp_exti.c" "bsp_gpio.c" "bsp_uart.c" "bsp_gptimer.c" "bsp_ledc.c" "bsp_i2c.c" "bsp_spi.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio esp_timer esp_driver_gptimer esp_driver_uart esp_driver_ledc esp_driver_i2c esp_driver_spi)
