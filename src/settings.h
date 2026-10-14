#pragma once
// Runtime settings, persisted in NVS. Changed over USB in idle mode; timer
// mode only ever reads them.

#include <stdint.h>

#include "detector.h"

struct Settings
{
    uint16_t freqMhz;
    uint8_t pilot;
    DetectorParams detector;
    char bindPhrase[64]; // ELRS bind phrase -> ESP-NOW UID
};

namespace settings
{

void begin(); // nvs_flash_init + load
Settings &get();
bool save();
void resetDefaults();

} // namespace settings
