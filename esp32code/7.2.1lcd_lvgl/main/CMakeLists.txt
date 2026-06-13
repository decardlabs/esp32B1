idf_component_register(SRCS "task_timer.c" "task_exti_prco.c" "task_led.c" "main.c" "task_uart.c" "task_ledc.c"
                       "task_oled.c" "task_ws2812.c" "task_dht11.c" "task_lcd_lvgl.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "include" ""    
                       REQUIRES esp_driver_gpio esp_timer bsp driver device lvgl)
