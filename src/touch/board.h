#pragma once
// Waveshare ESP32-C5-Touch-LCD-3.5 bring-up: the ST7796 display over SPI, the
// FT6336 touch panel and the CH32V003 IO expander (LCD/touch reset, backlight
// PWM) over I2C, and LVGL on top, turned to landscape 480x320.
// Pins and init order follow Waveshare's own board package for this board.

#include "lvgl.h"

namespace board
{

constexpr int WIDTH = 480;
constexpr int HEIGHT = 320;

// Starts the display, touch and LVGL's task. Returns the LVGL display, or
// nullptr if the panel didn't come up.
lv_display_t *begin();

// Backlight, 0-100 %.
void backlight(int percent);

// LVGL isn't thread-safe: hold this around any LVGL call made outside its task.
bool lock(uint32_t timeoutMs = 0);
void unlock();

} // namespace board
