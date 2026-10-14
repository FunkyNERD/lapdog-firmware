// LapDog - sits by the gate, barks on every lap.
//
// Idle mode: radio free, config over USB. Timer mode: radio dedicated to SDR
// capture, laps out over USB / BLE / ESP-NOW. Long-press BOOT to toggle.

#include <stdio.h>

#include "app.h"
#include "button.h"
#include "config.h"
#include "console.h"
#include "detector.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lap_frame.h"
#include "led.h"
#include "outputs.h"
#include "sdr.h"
#include "settings.h"
#include "vrxc_msp.h"

static const char *TAG = "lapdog";

static volatile bool isArmed = false;
static volatile bool simRequested = false;
static volatile bool rssiStream = false;

static uint8_t seq = 0;
static uint16_t lapCount = 0;

bool app::armed() { return isArmed; }
void app::simulatePass() { simRequested = true; }
void app::setRssiStream(bool on) { rssiStream = on; }

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

static lapframe::Frame frame(lapframe::Type type, uint32_t value)
{
    return {type, seq++, settings::get().pilot, value};
}

static void emit(const lapframe::Frame &f, bool radio)
{
    outputs::usb(f);
    if (!radio) return;
    sdr::end();
    outputs::radioBurst(f, lapCount);
    sdr::begin(settings::get().freqMhz); // the BT/Wi-Fi stack resets the PHY debug registers
}

static void setArmed(bool on, Detector &det, uint32_t now)
{
    isArmed = on;
    led::set(on ? led::ARMED : led::IDLE);
    if (on)
    {
        det = Detector(settings::get().detector);
        // TODO: measure the in-room baseline over a second or so instead.
        det.setBaseline(-90.0f);
        lapCount = 0;
        sdr::begin(settings::get().freqMhz);
    }
    else
        sdr::end();
    emit(frame(on ? lapframe::ARMED : lapframe::DISARMED, now), true);
    ESP_LOGI(TAG, "%s", on ? "armed" : "idle");
}

extern "C" void app_main()
{
    settings::begin();
    led::begin();
    button::begin();
    console::begin();

    uint8_t uid[6];
    vrxc::uidFromBindPhrase(settings::get().bindPhrase, uid);
    outputs::begin(uid);
    ESP_LOGI(TAG, "UID %02x:%02x:%02x:%02x:%02x:%02x", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);

    Detector det(settings::get().detector);
    uint32_t now = millis();
    setArmed(cfg::BOOT_ARMED, det, now);
    uint32_t lastHeartbeat = now;
    uint32_t lastRssi = now;

    while (true)
    {
        now = millis();

        switch (button::poll(now))
        {
        case button::LONG_PRESS: setArmed(!isArmed, det, now); break;
        case button::SHORT_PRESS: lapCount = 0; det.reset(); break; // new session
        case button::NONE: break;
        }

        if (simRequested)
        {
            simRequested = false;
            sdr::simulatePass(now);
        }

        if (isArmed)
        {
            uint32_t passMs, lapMs;
            if (det.feed(sdr::bandPowerDb(now), now, passMs, lapMs))
            {
                led::flash(now);
                // The first pass after arming starts lap 1 - nothing to report but the start.
                if (lapCount++ > 0) emit(frame(lapframe::LAP, lapMs), true);
                ESP_LOGI(TAG, "pass at %lu, lap %u %lu ms", (unsigned long)passMs, lapCount, (unsigned long)lapMs);
            }
            if (now - lastHeartbeat >= cfg::HEARTBEAT_MS)
            {
                lastHeartbeat = now;
                emit(frame(lapframe::HEARTBEAT, now), true);
            }
            if (rssiStream && now - lastRssi >= 50)
            {
                lastRssi = now;
                printf("R:%.1f\n", det.smoothed());
            }
        }

        led::update(now);
        vTaskDelay(pdMS_TO_TICKS(cfg::SAMPLE_PERIOD_MS));
    }
}
