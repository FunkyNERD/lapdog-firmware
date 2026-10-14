#include "outputs.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "vrxc_msp.h"

static const char *TAG = "out";

// VRxC_ELRS default lap/gap result row - RacePanel and the goggles read it there.
static const uint8_t OSD_LAP_RESULT_ROW = 15;

static uint8_t peer[6];
static bool espnowReady = false;

static void espnowBegin(const uint8_t uid[6])
{
    // Like the ELRS backpack: our own MAC *is* the bind-phrase UID, and the
    // goggles/RacePanel accept frames from it.
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, uid));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 2.4 GHz only: 5 GHz ESP-NOW (ch 149-165) lands on the FPV video band.
    ESP_ERROR_CHECK(esp_wifi_set_channel(cfg::ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());

    memcpy(peer, uid, 6);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, peer, 6);
    p.channel = cfg::ESPNOW_CHANNEL;
    p.ifidx = WIFI_IF_STA;
    ESP_ERROR_CHECK(esp_now_add_peer(&p));
    espnowReady = true;
}

void outputs::begin(const uint8_t uid[6])
{
    espnowBegin(uid);
    // TODO: NimBLE host init for non-connectable advertising bursts.
}

void outputs::usb(const lapframe::Frame &f)
{
    uint8_t b[lapframe::SIZE];
    lapframe::encode(f, b);
    printf("F:");
    for (size_t i = 0; i < sizeof(b); i++) printf("%02X", b[i]);
    printf("\n");
}

static void espnowLap(uint32_t lapMs, uint16_t lapNumber)
{
    if (!espnowReady) return;
    char text[vrxc::OSD_COLS + 1];
    snprintf(text, sizeof(text), "L%u:%lu.%03lu", lapNumber, (unsigned long)(lapMs / 1000),
             (unsigned long)(lapMs % 1000));
    uint8_t frame[80];
    size_t n = vrxc::osdText(frame, sizeof(frame), OSD_LAP_RESULT_ROW, 0, text);
    esp_now_send(peer, frame, n);
    n = vrxc::osdDisplay(frame, sizeof(frame));
    esp_now_send(peer, frame, n);
}

static void bleBurst(const lapframe::Frame &f)
{
    // TODO: advertise [company id 0xFFFF LE][frame] as manufacturer data,
    // non-connectable, every cfg::BLE_ADV_INTERVAL_MS for cfg::BLE_BURST_MS.
    (void)f;
    ESP_LOGD(TAG, "ble burst (stub)");
}

void outputs::radioBurst(const lapframe::Frame &f, uint16_t lapNumber)
{
    if (f.type == lapframe::LAP && lapNumber > 0) espnowLap(f.value, lapNumber);
    bleBurst(f);
}
