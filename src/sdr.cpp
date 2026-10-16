#include "sdr.h"

#include <math.h>
#include <stdio.h>

#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#if CONFIG_IDF_TARGET_ESP32C5
#include "esp_sdr/esp_sdr_c5.h"
#endif

static const char *TAG = "sdr";
static const uint32_t SIM_PASS_MS = 300;
static const float SIM_PASS_DB = 25.0f;

static uint32_t simStartMs = 0;
static bool simActive = false;
static bool isRunning = false;
static float lastDb = cfg::SDR_STUB_FLOOR_DB;
static uint32_t errors = 0;
static unsigned bandwidthMhz = cfg::SDR_BANDWIDTH_MHZ;

// A synthetic gate pass: a half-sine bump on whatever the receiver reads.
static float simBump(uint32_t nowMs)
{
    if (!simActive) return 0;
    uint32_t t = nowMs - simStartMs;
    if (t >= SIM_PASS_MS)
    {
        simActive = false;
        return 0;
    }
    return SIM_PASS_DB * sinf((float)M_PI * (float)t / SIM_PASS_MS);
}

void sdr::simulatePass(uint32_t nowMs)
{
    simStartMs = nowMs;
    simActive = true;
}

bool sdr::running() { return isRunning; }
void sdr::end() { isRunning = false; }
uint32_t sdr::captureErrors() { return errors; }
float sdr::levelDbfs() { return lastDb; }
unsigned sdr::bandwidth() { return bandwidthMhz; }

#if CONFIG_IDF_TARGET_ESP32C5

// Gain and bandwidth changes come from the console task; they're applied here,
// on the loop that owns the radio, between captures.
static volatile int wantGain = cfg::SDR_GAIN;
static volatile unsigned wantBandwidth = cfg::SDR_BANDWIDTH_MHZ;
static volatile bool changed = true;

static void applyChanges()
{
    if (!changed) return;
    changed = false;
    esp_sdr_set_bandwidth(wantBandwidth);
    esp_sdr_set_gain(wantGain);
}

bool sdr::available() { return true; }

bool sdr::begin(uint16_t freqMhz)
{
    // The RF test library logs a line per capture under "phy".
    esp_log_level_set("phy", ESP_LOG_WARN);
    // As ESP-SDR does: promiscuous keeps the Wi-Fi receive path running.
    static bool promiscuous = false;
    if (!promiscuous) promiscuous = esp_wifi_set_promiscuous(true) == ESP_OK;
    applyChanges();
    esp_sdr_set_frequency(freqMhz);
    esp_sdr_invalidate(); // after a burst the BT/Wi-Fi stacks have reset the PHY
    esp_sdr_prepare();
    isRunning = true;
    return true;
}

// Mean power of one snapshot, DC (the receiver's own offset) removed, in dB
// relative to a full-scale 10-bit sample.
static float snapshotDb(const uint32_t *iq, unsigned n)
{
    int32_t sumI = 0, sumQ = 0;
    for (unsigned j = 0; j < n; j++)
    {
        sumI += ((int32_t)(iq[j] << 22)) >> 22;
        sumQ += ((int32_t)(iq[j] << 12)) >> 22;
    }
    float mi = (float)sumI / n, mq = (float)sumQ / n;
    float p = 0;
    for (unsigned j = 0; j < n; j++)
    {
        float i = (float)(((int32_t)(iq[j] << 22)) >> 22) - mi;
        float q = (float)(((int32_t)(iq[j] << 12)) >> 22) - mq;
        p += i * i + q * q;
    }
    p /= (float)n * 511.0f * 511.0f;
    return 10.0f * log10f(p + 1e-12f);
}

static volatile bool dumpNext = false;
static uint32_t captures = 0, captureUs = 0;
void sdr::stats(uint32_t &n, uint32_t &avgUs, uint32_t &recals)
{
    n = captures;
    avgUs = captures ? captureUs / captures : 0;
    recals = esp_sdr_recalibrations;
    captures = captureUs = 0;
}
void sdr::dump() { dumpNext = true; }

float sdr::bandPowerDb(uint32_t nowMs)
{
    if (isRunning)
    {
        applyChanges();
        const uint32_t *iq;
        int64_t t0 = esp_timer_get_time();
        bool ok = esp_sdr_capture(cfg::SDR_SAMPLES, cfg::SDR_RATE, &iq);
        captureUs += (uint32_t)(esp_timer_get_time() - t0);
        captures++;
        if (dumpNext)
        {
            dumpNext = false;
            printf("D:%s", ok ? "ok" : "fail");
            for (int i = 0; i < 16; i++) printf(" %08lx", (unsigned long)iq[i]);
            printf("\n");
        }
        if (ok) lastDb = snapshotDb(iq, cfg::SDR_SAMPLES);
        else if (++errors % 100 == 1) ESP_LOGW(TAG, "incomplete capture (%lu so far)", (unsigned long)errors);
    }
    return lastDb + simBump(nowMs);
}

void sdr::setGain(int index)
{
    wantGain = index < 0 ? -1 : index;
    changed = true;
}
int sdr::gain() { return wantGain; }
unsigned sdr::gainMax() { return esp_sdr_gain_max(); }

bool sdr::setBandwidth(unsigned mhz)
{
    if (mhz && (mhz < 11 || mhz > 48)) return false;
    bandwidthMhz = wantBandwidth = mhz;
    changed = true;
    return true;
}

#else // no receiver: a flat floor with a little noise

bool sdr::available() { return false; }

bool sdr::begin(uint16_t freqMhz)
{
    ESP_LOGD(TAG, "no SDR on this chip: not tuning to %u MHz", freqMhz);
    isRunning = true;
    return true;
}

float sdr::bandPowerDb(uint32_t nowMs)
{
    float noise = ((float)(esp_random() % 1000) / 1000.0f - 0.5f) * 2.0f; // +/-1 dB
    return cfg::SDR_STUB_FLOOR_DB + noise + simBump(nowMs);
}

void sdr::setGain(int) {}
void sdr::dump() {}
void sdr::stats(uint32_t &n, uint32_t &avgUs, uint32_t &recals) { n = avgUs = recals = 0; }
int sdr::gain() { return -1; }
unsigned sdr::gainMax() { return 0; }
bool sdr::setBandwidth(unsigned) { return false; }

#endif
