# Waveshare ESP32-C5-Touch-LCD-3.5 support for the c5_touch build: the
# managed components it needs (idf_component.yml) and Waveshare's FT6336 touch
# driver, vendored from their demo repo (Apache-2.0, see NOTICE).
idf_component_register(SRCS "esp_lcd_touch_ft6336.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c)
