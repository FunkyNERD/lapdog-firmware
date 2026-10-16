#pragma once
// Status OLED (72x40 SSD1306) on boards that have one - see cfg::HAS_OLED.
// It follows the session from the same frames the app gets, so it shows
// what the app would: heat, the lap being flown, its running time and the
// last lap. The screen inverts briefly on each lap. Drawn from its own
// low-priority task, so the I2C writes never hold up the detector.
// Without an OLED every call is a no-op. On the touch board (LAPDOG_TOUCH)
// the same two calls drive the LVGL screens in src/touch/ instead.

#include "lap_frame.h"

namespace display
{

void begin();
void frame(const lapframe::Frame &f); // every frame the session sends

} // namespace display
