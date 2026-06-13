idf_component_register(
    SRCS "main.c" "task_key_led.c" "xl9555_io.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c
)
