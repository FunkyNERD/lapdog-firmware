#pragma once
// Runtime settings, persisted in NVS. Changed over USB in idle mode; timer
// mode only ever reads them. The struct and its text form are in
// include/timer_settings.h.

#include <stdint.h>

#include "timer_settings.h"

namespace settings
{

void begin(); // nvs_flash_init + load
Settings &get();
bool save();
void resetDefaults();

// The heat number the next session gets. Kept apart from the settings blob:
// the timer advances it itself on every arm, without a "save".
uint16_t nextHeat();
void setNextHeat(uint16_t heat); // persisted immediately

} // namespace settings
