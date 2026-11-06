#pragma once
// Status LED, readable with goggles on: slow blink = idle, solid = armed,
// quick flash = lap, double blink = calibrating.

#include <stdint.h>

namespace led
{

enum Pattern { IDLE, ARMED, CALIBRATING };

void begin();
void set(Pattern p);
void flash(uint32_t nowMs); // one-shot lap flash over the current pattern
void update(uint32_t nowMs);

} // namespace led
