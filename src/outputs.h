#pragma once
// Lap/event outputs. The C5 has one RF chain, so wireless outputs follow
// capture -> lap detected -> brief transmit burst -> resume capture; the
// caller stops the SDR around radioBurst().

#include <stdint.h>

#include "lap_frame.h"

namespace outputs
{

void begin(const uint8_t uid[6]);

// USB (CDC): always, immediately - doesn't touch the radio.
void usb(const lapframe::Frame &f);

// BLE advert burst + ESP-NOW OSD message. Blocks for the burst.
void radioBurst(const lapframe::Frame &f, uint16_t lapNumber);

} // namespace outputs
