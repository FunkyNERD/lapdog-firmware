#include "settings.h"

#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sound_choice.h"

static const char *TAG = "settings";
static const char *NVS_NS = "lapdog";
static const uint32_t SETTINGS_VERSION = 4; // bump when the struct layout changes

static Settings current;
static uint16_t heat = 1;
static bool heatUnsaved = false;
static settings::Sound snd = {sound::DEFAULT_CHOICE, sound::DEFAULT_VOLUME, false};

void settings::resetDefaults()
{
    snd = {sound::DEFAULT_CHOICE, sound::DEFAULT_VOLUME, false};
    current = timer_settings::defaults(
        cfg::DEFAULT_FREQ_MHZ, {cfg::DEFAULT_EMA_ALPHA, cfg::DEFAULT_ENTER_DB, cfg::DEFAULT_EXIT_DB, cfg::DEFAULT_MIN_LAP_MS,
                                       cfg::DEFAULT_MAX_PEAK_MS});
}

void settings::begin()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    resetDefaults();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8;
    if (nvs_get_u8(h, "snd", &u8) == ESP_OK && u8 < sound::CHOICES) snd.choice = u8;
    if (nvs_get_u8(h, "vol", &u8) == ESP_OK && u8 <= 100) snd.volume = u8;
    if (nvs_get_u8(h, "cue", &u8) == ESP_OK) snd.cues = u8 != 0;
    uint32_t version = 0;
    size_t len = sizeof(current);
    if (nvs_get_u32(h, "ver", &version) == ESP_OK && (version == SETTINGS_VERSION || version == 3) &&
        nvs_get_blob(h, "cfg", &current, &len) == ESP_OK && len == sizeof(current))
    {
        // v3 had one link (0 BLE, 1 ESP-NOW) in the byte that's now the links bitmask.
        if (version == 3) current.links = current.links == 1 ? LINK_ESPNOW : LINK_BLE;
        ESP_LOGI(TAG, "loaded");
    }
    else
        resetDefaults();
    if (nvs_get_u16(h, "heat", &heat) != ESP_OK || heat == 0) heat = 1;
    nvs_close(h);
}

Settings &settings::get() { return current; }
settings::Sound &settings::sound() { return snd; }

bool settings::save()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (heatUnsaved && nvs_set_u16(h, "heat", heat) == ESP_OK) heatUnsaved = false;
    nvs_set_u8(h, "snd", snd.choice);
    nvs_set_u8(h, "vol", snd.volume);
    nvs_set_u8(h, "cue", snd.cues);
    bool ok = nvs_set_u32(h, "ver", SETTINGS_VERSION) == ESP_OK &&
              nvs_set_blob(h, "cfg", &current, sizeof(current)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

uint16_t settings::nextHeat() { return heat; }

void settings::setNextHeatUnsaved(uint16_t next)
{
    heat = next ? next : 1;
    heatUnsaved = true;
}

void settings::setNextHeat(uint16_t next)
{
    heat = next ? next : 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "heat", heat);
    nvs_commit(h);
    nvs_close(h);
}
