idf_component_register(SRCS "ws2812b.c" "xl9555.c"
                    INCLUDE_DIRS "include"
                    REQUIRES bsp esp_driver_i2c led_strip)
                
