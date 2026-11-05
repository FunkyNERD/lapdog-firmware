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
void setNextHeatUnsaved(uint16_t heat); // persisted by the next save()

// The touch timer's lap sound. Kept apart from the Settings blob (a new field would
// change its size and reset every saved setting): their own NVS keys, written by
// save() with the rest.
struct Sound
{
    uint8_t choice;  // sound::Choice
    uint8_t volume;  // percent
    bool cues;       // a tone for GO and for the finish in a race
};
Sound &sound();

} // namespace settings
