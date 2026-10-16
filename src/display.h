#pragma once
// Status OLED (72x40 SSD1306) on boards that have one - see cfg::HAS_OLED.
// It follows the session from the same frames the app gets, so it shows
// what the app would: heat, the lap being flown, its running time and the
// last lap. The screen inverts briefly on each lap. Drawn from its own
// low-priority task, so the I2C writes never hold up the detector.
// Without an OLED every call is a no-op. On the touch board (LAPDOG_TOUCH)
// the same calls drive the LVGL screens in src/touch/ instead, and start-up
// runs behind a boot splash (the other boards have no splash: those calls are
// no-ops there).

#include "lap_frame.h"

namespace display
{

void begin();                         // after settings are loaded
void frame(const lapframe::Frame &f); // every frame the session sends

// Boot splash: splash() first thing at boot, bootStep() before each piece of
// start-up work, bootDone() when it's all done. None of them wait: the splash
// animates and paces itself, then hands over to the main screen on its own.
void splash();
void bootStep(const char *label); // a string literal: it's kept, not copied
void bootDone();

} // namespace display
