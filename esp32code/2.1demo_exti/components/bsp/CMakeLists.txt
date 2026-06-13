idf_component_register(SRCS "bsp_exti.c" "bsp_gpio.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio driver esp_timer)

                    
component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
