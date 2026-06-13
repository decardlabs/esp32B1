idf_component_register(SRCS "main.c" "esp_hid_gap.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_hid esp_driver_gpio bsp device)
