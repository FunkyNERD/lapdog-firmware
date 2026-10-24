cmake_minimum_required(VERSION 3.16.0)

# env:c5_touch (Waveshare ESP32-C5-Touch-LCD-3.5) passes -DLAPDOG_TOUCH=1: add
# the board's component, which brings in LVGL and the display/touch drivers.
# The other builds never see it.
if(LAPDOG_TOUCH)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/c5_touch")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(LAPDOG_TOUCH)
    # Website sync resolves one 22-character hostname. lwIP's DNS cache is static
    # data in internal RAM, below the SDR's capture bank: 2 x 64-byte names instead
    # of 4 x 256 (not in menuconfig, so a define for every component).
    idf_build_set_property(COMPILE_DEFINITIONS "DNS_TABLE_SIZE=2" APPEND)
    idf_build_set_property(COMPILE_DEFINITIONS "DNS_MAX_NAME_LENGTH=64" APPEND)
endif()

project(lapdog)
