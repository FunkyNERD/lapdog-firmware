#pragma once
// Lap/event outputs. USB always carries every frame. The one RF chain does
// either BLE advert bursts (to the LapDog app) or ESP-NOW (VRxC_ELRS OSD to
// goggles/RacePanel) - never both, chosen by Settings::link.
//
// Wireless follows capture -> event -> brief transmit burst -> resume
// capture: the sink queues a burst and flush() stops the SDR, sends, and
// restarts it (the BT/Wi-Fi stack resets the PHY debug registers).

#include <stdint.h>

#include "race.h"
#include "timer_settings.h"

namespace outputs
{

// Brings up the selected link and tears down the other. Cheap when nothing
// changed. Call with the SDR stopped (idle), e.g. right before arming.
void setLink(Link link, const uint8_t uid[6]);

// The session's sink: frames to USB immediately, radio on flush().
race::Sink &sink();

} // namespace outputs
