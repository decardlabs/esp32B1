idf_component_register(SRCS "app_wifi_store.c" "app_wifi_portal.c" "task_timer.c" "task_exti_prco.c" "task_led.c" "main.c" "task_uart.c" "task_ledc.c"
                            "task_oled.c" "task_ws2812.c" "task_dht11.c" "task_lcd_lvgl.c" "task_lcd_lvgl_camera.c" "task_xiaozhi.c" "task_sdcard.c"
                            "xiaozhi_activation.c" "xiaozhi_ui_status.c" "lv_font_xiaozhi_cn_16.c"
                            "face_recognition_service.cpp"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "include" ""
                       REQUIRES esp_driver_gpio esp_timer bsp driver device lvgl json nvs_flash esp_event esp_netif esp_wifi
                                mbedtls app_update esp_http_client spiffs esp_http_server)
