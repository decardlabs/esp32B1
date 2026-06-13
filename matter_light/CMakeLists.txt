cmake_minimum_required(VERSION 3.5)

# Set ESP_MATTER_PATH (must be an env var for ESP-Matter components)
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    set(ENV{ESP_MATTER_PATH} "$ENV{HOME}/esp/esp-matter")
endif()

set(PROJECT_VER "1.0")
set(PROJECT_VER_NUMBER 1)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# Must be included before project()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Add ESP-Matter core component directories (not device_hal - we have custom BSP)
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/components"
    "${MATTER_SDK_PATH}/config/esp32/components")

# Set C++ standard for Matter CHIP components
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-DCHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

project(matter_light)
