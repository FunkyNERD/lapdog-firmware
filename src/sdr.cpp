#include "sdr.h"

#include <math.h>

#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "sdr";
static const float NOISE_FLOOR_DB = -90.0f;
static const uint32_t SIM_PASS_MS = 300;

static uint32_t simStartMs = 0;
static bool simActive = false;
static bool isRunning = false;

bool sdr::begin(uint16_t freqMhz)
{
    ESP_LOGD(TAG, "stub: not tuning to %u MHz (ESP-SDR not integrated)", freqMhz);
    isRunning = true;
    return true;
}

void sdr::end() { isRunning = false; }

bool sdr::running() { return isRunning; }

void sdr::simulatePass(uint32_t nowMs)
{
    simStartMs = nowMs;
    simActive = true;
}

float sdr::bandPowerDb(uint32_t nowMs)
{
    float noise = ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * 2.0f; // +/-1 dB
    float db = NOISE_FLOOR_DB + noise;
    if (simActive)
    {
        uint32_t t = nowMs - simStartMs;
        if (t >= SIM_PASS_MS) simActive = false;
        else db += 25.0f * sinf((float)M_PI * (float)t / SIM_PASS_MS); // 25 dB bump
    }
    return db;
}
