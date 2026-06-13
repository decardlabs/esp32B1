idf_component_register(
    SRC_DIRS "."
    PRIV_INCLUDE_DIRS "." "include"
    REQUIRES
        bsp
        device
        esp_matter
        lvgl
        led_strip
        nvs_flash
        esp_netif
        esp_event
        mdns
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
