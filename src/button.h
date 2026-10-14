#pragma once
// BOOT button: long press (cfg::LONG_PRESS_MS) toggles idle/timer mode,
// short press marks a new session. Polled from the main loop.

#include <stdint.h>

namespace button
{

enum Event { NONE, SHORT_PRESS, LONG_PRESS };

void begin();
Event poll(uint32_t nowMs);

} // namespace button
