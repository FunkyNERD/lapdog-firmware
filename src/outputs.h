#pragma once
// Lap/event outputs. USB always carries every frame. Over the air, BLE advert
// bursts (to the LapDog app) and/or ESP-NOW (VRxC_ELRS OSD to goggles and
// RacePanel), chosen by Settings::links. With both, ESP-NOW goes first - the
// goggles are what the pilot sees - and BLE follows (~0.5 s later; lap times
// are timed on the timer, so the delay doesn't change them).
//
// Wireless follows capture -> event -> brief transmit burst -> resume
// capture: the sink queues a burst and flush() stops the SDR, sends, and
// restarts it (the BT/Wi-Fi stack resets the PHY debug registers).

#include <stdint.h>

#include "race.h"
#include "timer_settings.h"

namespace outputs
{

// Brings up the selected links (Link bits) and tears down the rest. Cheap when
// nothing changed. Call with the SDR stopped (idle), e.g. right before arming.
void setLinks(uint8_t links, const uint8_t uid[6]);

// The session's sink: frames to USB immediately, radio on flush().
race::Sink &sink();

} // namespace outputs
