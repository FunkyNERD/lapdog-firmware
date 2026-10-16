#pragma once
// ESP32-C5 raw I/Q capture from the radio's debug path - the receive side of
// ESP-SDR (ESPARGOS), ported without its serial protocol. See esp_sdr_c5.c.
//
// The Wi-Fi driver must be started (it powers and calibrates the PHY) before
// the first capture. Captures take the radio from Wi-Fi/BLE: call
// esp_sdr_prepare() again after any transmit, as the stacks reset the PHY.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sample rates for esp_sdr_capture(): index -> MS/s.
enum
{
    ESP_SDR_RATE_80 = 0,
    ESP_SDR_RATE_40 = 1,
    ESP_SDR_RATE_20 = 2,
    ESP_SDR_RATE_10 = 3,
    ESP_SDR_RATE_8 = 4,
    ESP_SDR_RATE_4 = 5,
};

#define ESP_SDR_MAX_SAMPLES 16380u

// Receive frequency in MHz (100-6000; the C5's front end covers 4.8-6.0 GHz on 5 GHz).
// Takes effect on the next esp_sdr_prepare()/capture, re-measuring RX DC there.
void esp_sdr_set_frequency(unsigned mhz);

// Approximate analog bandwidth in MHz (11-48), or 0 for the widest setting.
bool esp_sdr_set_bandwidth(unsigned mhz);

// Manual receive gain index (0..esp_sdr_gain_max()), or -1 for the hardware AGC.
// LapDog uses a fixed gain: the AGC would flatten the gate-pass peak.
void esp_sdr_set_gain(int index);
int esp_sdr_gain(void);
unsigned esp_sdr_gain_max(void); // 0 until the PHY has been tuned once

// Puts the radio in receive at the set frequency (recalibrating RX DC first
// when the frequency changed or the PHY replaced its DC tables).
void esp_sdr_prepare(void);
// Forgets that the radio is set up, so the next prepare redoes it all (after BLE/ESP-NOW).
void esp_sdr_invalidate(void);

// One snapshot of n complex samples (256..ESP_SDR_MAX_SAMPLES) at rate (ESP_SDR_RATE_*).
// *iq points at n words: I in bits 0-9, Q in bits 10-19, both signed 10-bit.
// The buffer is only valid until the next capture.
bool esp_sdr_capture(unsigned n, unsigned rate, const uint32_t **iq);

#ifdef __cplusplus
}
#endif
