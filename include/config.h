#pragma once
// Board pins and tuning defaults. Runtime settings (channel, thresholds,
// bind phrase) live in NVS - see settings.h; these are only the first-boot
// defaults and the things that are fixed per board.

#include <stdint.h>

#include "driver/gpio.h"
#include "sdkconfig.h"

namespace cfg
{

// --- Pins ----------------------------------------------------------------------
// BOOT is a strapping pin on both chips: fine to read once running, just don't
// hold it at power-on.
#if CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3 SuperMini - test build only (env:c3_test). No SDR: laps come from
// `sim`. For testing ESP-NOW, BLE, the USB console and the app.
constexpr gpio_num_t PIN_BUTTON = GPIO_NUM_9;
constexpr gpio_num_t PIN_LED = GPIO_NUM_8; // blue onboard LED, lit when low
constexpr bool LED_ACTIVE_LOW = true;
#else
// Mini ESP32-C5 dev board - the real timer.
constexpr gpio_num_t PIN_BUTTON = GPIO_NUM_28;
// TODO: confirm against the mini board's schematic once it arrives.
constexpr gpio_num_t PIN_LED = GPIO_NUM_27;
constexpr bool LED_ACTIVE_LOW = false;
#endif

// --- Button ------------------------------------------------------------------
constexpr uint32_t LONG_PRESS_MS = 1000; // toggles idle <-> timer
constexpr uint32_t DEBOUNCE_MS = 30;

// --- Detection defaults ------------------------------------------------------
constexpr uint16_t DEFAULT_FREQ_MHZ = 5800;  // mid-band; band edges are less sensitive
constexpr float DEFAULT_EMA_ALPHA = 0.2f;
constexpr float DEFAULT_ENTER_DB = 10.0f;    // above the in-room baseline
constexpr float DEFAULT_EXIT_DB = 5.0f;
constexpr uint32_t DEFAULT_MIN_LAP_MS = 3000;  // shorter "laps" are a crash or bounce at the gate
constexpr uint32_t DEFAULT_MAX_PEAK_MS = 1500; // longer peaks are a quad parked at the gate
constexpr uint32_t SAMPLE_PERIOD_MS = 5;     // ~200 Hz band-power rate

// --- Outputs -----------------------------------------------------------------
constexpr uint32_t HEARTBEAT_MS = 5000;      // while armed
constexpr uint8_t ESPNOW_CHANNEL = 1;        // 2.4 GHz only - never 5 GHz (overlaps the video band)
constexpr uint32_t ESPNOW_FRAME_GAP_MS = 8;  // between OSD frames, as elrs-race-test sends them
constexpr uint32_t BLE_BURST_MS = 400;        // per burst, shared by its frames
constexpr uint32_t BLE_FRAME_MIN_MS = 60;     // but each frame gets at least this (~3 adverts)
constexpr uint32_t BLE_ADV_INTERVAL_MS = 20;

// Power-on mode: idle is safer while developing. BOOT arms Settings::mode.
constexpr bool BOOT_ARMED = false;

} // namespace cfg
