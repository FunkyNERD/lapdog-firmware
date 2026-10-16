#pragma once
// Band power from the C5's radio used as an SDR - the receive path of ESP-SDR
// (ESPARGOS), ported in src/esp_sdr/. Each bandPowerDb() takes one short I/Q
// snapshot at the video channel's frequency and returns its power in dBFS.
// The detector compares that against a tracked in-room baseline, so only
// changes in level matter, not the absolute figure.
//
// The radio does one thing at a time: end() before a BLE/ESP-NOW burst,
// begin() after (the stacks reset the PHY's debug configuration).
//
// On the C3 (2.4 GHz only) there's no SDR: bandPowerDb() is a flat noise
// floor. On every chip, simulatePass() (the USB "sim" command) adds a
// synthetic gate pass on top, so the detector -> output path can be tested
// without a quad.

#include <stdint.h>

namespace sdr
{

bool begin(uint16_t freqMhz);
void end(); // release the radio for a transmit burst
bool running();
float bandPowerDb(uint32_t nowMs);
void simulatePass(uint32_t nowMs);

// Whether this chip has a real receiver (the C5).
bool available();

// Receive tuning, for bring-up from the console ("sdr ..."). Not saved.
void setGain(int index); // manual gain index, -1 = hardware AGC
int gain();
unsigned gainMax(); // 0 until the radio has been tuned once
bool setBandwidth(unsigned mhz); // 11-48 MHz, 0 = widest
unsigned bandwidth();
uint32_t captureErrors(); // snapshots that came back incomplete, since boot
float levelDbfs();        // the last snapshot's power, dB relative to full scale
void stats(uint32_t &captures, uint32_t &avgUs, uint32_t &recalibrations); // since the last call
void dump();              // print the next snapshot's first raw words ("D:" line)

} // namespace sdr
