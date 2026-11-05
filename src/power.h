#pragma once
// Battery and USB power on the touch timer, from the board's AXP2101 power-management
// chip (read over I2C, board.h). Read-only: the charger keeps the chip's own settings.
//
// Poll from the UI side every few seconds, never from the timing loop.

#include <stdint.h>

namespace power
{

struct State
{
    bool valid;     // the chip answered
    bool battery;   // a battery is fitted
    bool vbus;      // USB power present
    bool charging;  // the battery is taking charge
    int8_t percent; // 0-100, steadied for the screen (-1 = no battery or no reading)
    int mv;         // battery voltage, 0 if unknown
    uint8_t chargerState; // 0 trickle, 1 pre, 2 constant current, 3 constant voltage, 4 done, 5 not charging
};

// Enables the chip's voltage ADC and fuel gauge. After board::begin().
void begin();
// Reads the chip. Cheap (a few I2C transfers); every 5 s is plenty.
void poll();
State state();
// The console's "power": the readings, as S: lines.
void printInfo();

} // namespace power
