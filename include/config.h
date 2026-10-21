#pragma once
// Board pins and tuning defaults. Runtime settings (channel, thresholds,
// bind phrase) live in NVS - see settings.h; these are only the first-boot
// defaults and the things that are fixed per board.

#include <stdint.h>

#include "driver/gpio.h"
#include "sdkconfig.h"

// Firmware version. Releases set it from the tag (tools/release.sh keeps this default in step),
// so a USB build of a tagged commit reports the same version.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.3.2"
#endif

namespace cfg
{

// --- Pins ----------------------------------------------------------------------
// BOOT is a strapping pin on both chips: fine to read once running, just don't
// hold it at power-on.
#if CONFIG_IDF_TARGET_ESP32C3
// 01Space ESP32-C3 0.42" OLED board - test build only (env:c3_oled), the board
// elrs-race-test runs on. No SDR: laps come from `sim`. For testing ESP-NOW,
// BLE, the USB console and the app. Its LED is a WS2812 on GPIO 2, not driven
// here: the OLED shows the state instead.
constexpr gpio_num_t PIN_BUTTON = GPIO_NUM_9;
constexpr gpio_num_t PIN_LED = GPIO_NUM_NC;
constexpr bool LED_ACTIVE_LOW = false;
constexpr gpio_num_t PIN_OLED_SDA = GPIO_NUM_5;
constexpr gpio_num_t PIN_OLED_SCL = GPIO_NUM_6;
#elif LAPDOG_TOUCH
// Waveshare ESP32-C5-Touch-LCD-3.5 (env:c5_touch). No plain LED: the screen shows
// the state, and GPIO 27 is its I2C data line (touch, IO expander).
constexpr gpio_num_t PIN_BUTTON = GPIO_NUM_28;
constexpr gpio_num_t PIN_LED = GPIO_NUM_NC;
constexpr bool LED_ACTIVE_LOW = false;
#else
// Mini ESP32-C5 dev board - the real timer.
constexpr gpio_num_t PIN_BUTTON = GPIO_NUM_28;
// TODO: confirm against the mini board's schematic once it arrives.
constexpr gpio_num_t PIN_LED = GPIO_NUM_27;
constexpr bool LED_ACTIVE_LOW = false;
#endif

// Status OLED (SSD1306 72x40), on boards that have one. Build with
// -DLAPDOG_OLED=0 to leave it dark.
#ifndef LAPDOG_OLED
#define LAPDOG_OLED 0
#endif
constexpr bool HAS_OLED = LAPDOG_OLED;
constexpr uint32_t OLED_FRAME_MS = 100;

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
constexpr unsigned TIMING_TASK_PRIORITY = 6;  // above the console (5) and the touch UI (3)
// The in-room baseline follows the level outside peaks: down quickly, up by this
// fraction per sample (~2.5 s time constant at 200 Hz), never during a peak.
constexpr float BASELINE_RISE = 0.002f;

// --- SDR (C5) -----------------------------------------------------------------
// One snapshot per sample: SDR_SAMPLES complex samples at SDR_RATE (0..5 = 80,
// 40, 20, 10, 8, 4 MS/s) through an analog filter about SDR_BANDWIDTH_MHZ wide
// (an analog video channel is ~18 MHz). Fixed gain: the AGC would flatten the
// gate-pass peak. All tunable from the console ("sdr ...") during bring-up.
#if CONFIG_IDF_TARGET_ESP32C5
constexpr bool HAS_SDR = true;
#else
constexpr bool HAS_SDR = false;
#endif
constexpr unsigned SDR_SAMPLES = 1024;
constexpr unsigned SDR_RATE = 2; // 20 MS/s: ~51 us per snapshot
constexpr unsigned SDR_BANDWIDTH_MHZ = 20;
constexpr int SDR_GAIN = 40;     // PHY gain index; -1 = hardware AGC
constexpr float SDR_STUB_FLOOR_DB = -90.0f; // what the C3's stand-in reads

// --- Outputs -----------------------------------------------------------------
constexpr uint32_t HEARTBEAT_MS = 5000;      // while armed
// After a disarm, keep saying "idle" for a while. The DISARMED burst is one short
// burst, and an idle timer is otherwise silent, so a phone that missed it would keep
// timing the heat forever; any one of these ends it there.
constexpr uint32_t IDLE_HEARTBEAT_MS = 2000;
constexpr uint32_t IDLE_HEARTBEATS_FOR_MS = 16000;
constexpr uint8_t ESPNOW_CHANNEL = 1;        // 2.4 GHz only - never 5 GHz (overlaps the video band)
constexpr uint32_t ESPNOW_FRAME_GAP_MS = 8;  // between OSD frames, as elrs-race-test sends them
// A burst's frames go out one after another. The ones the app acts on (LAP, ARMED,
// DISARMED, RACE, IGNORED, HEARTBEAT) get most of the airtime; the OSD text (TEXT)
// gets a short slot each. At a 20 ms interval that's ~7 adverts of each key frame and
// ~2 of each text frame.
//
// Then the key frames (bar heartbeats) go out again as short echoes at each of
// BLE_ECHO_AFTER_MS. Phones only listen part of the time (a Xiaomi on Wi-Fi heard
// 20-30% of adverts, whatever the scan filter), in gaps that a repeat inside the same
// burst falls into; spread over ~2.5 s, one of four windows nearly always lands. The
// last echo stays inside the 3 s minimum lap, so it never overlaps the next pass.
constexpr uint32_t BLE_KEY_FRAME_MS = 140;
constexpr uint32_t BLE_TEXT_FRAME_MS = 40;
constexpr uint32_t BLE_ECHO_AFTER_MS[] = {700, 1500, 2400};
constexpr uint32_t BLE_ECHO_FRAME_MS = 100;
constexpr uint32_t BLE_ADV_INTERVAL_MS = 20;

// Power-on mode: idle is safer while developing. BOOT arms Settings::mode.
constexpr bool BOOT_ARMED = false;

} // namespace cfg
