#pragma once
// Band power from the C5's radio used as an SDR (ESP-SDR / ESPARGOS).
//
// STUB: ESP-SDR isn't integrated yet. bandPowerDb() returns a noise floor,
// plus a synthetic gate pass when simulatePass() has been called (the USB
// "sim" command), so the detector -> output path can be tested end-to-end.
//
// TODO (first steps in CLAUDE.md):
//   - pull in ESP-SDR's capture path, tune to freqMhz, compute band power on-chip
//   - fixed gain (AGC would flatten the peak)
//   - re-apply the PHY debug config after every BLE/ESP-NOW burst (begin() again)

#include <stdint.h>

namespace sdr
{

bool begin(uint16_t freqMhz);
void end(); // release the radio for a transmit burst
float bandPowerDb(uint32_t nowMs);
void simulatePass(uint32_t nowMs);

} // namespace sdr
