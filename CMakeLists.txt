cmake_minimum_required(VERSION 3.16.0)

# env:c5_touch (Waveshare ESP32-C5-Touch-LCD-3.5) passes -DLAPDOG_TOUCH=1: add
# the board's component, which brings in LVGL and the display/touch drivers.
# The other builds never see it.
if(LAPDOG_TOUCH)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards/c5_touch")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lapdog)
