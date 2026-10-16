FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.cpp)

if(IDF_TARGET STREQUAL "esp32c5")
    # The ESP-SDR receive path (src/esp_sdr/): raw I/Q from the radio's debug
    # capture, which needs the PHY's RF test library and debug hooks.
    list(APPEND app_sources ${CMAKE_SOURCE_DIR}/src/esp_sdr/esp_sdr_c5.c)
endif()

# No REQUIRES: like an IDF "main" component, this one sees every component in
# the build (lapdog_touch too, when LAPDOG_TOUCH adds it).
idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "../include")

if(LAPDOG_TOUCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LAPDOG_TOUCH=1)
endif()

if(IDF_TARGET STREQUAL "esp32c5")
    # Point the PHY's channel setup at our frequency during RX DC measurement,
    # and count DC re-measurements (esp_sdr_c5.c); keep static data clear of
    # the SRAM bank the modem writes samples into.
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=phy_chip_set_chan_ana"
        "-Wl,--wrap=phy_set_rx_gain_cal_dc"
        "-T${CMAKE_CURRENT_LIST_DIR}/esp_sdr/sram_guard.ld")
endif()
