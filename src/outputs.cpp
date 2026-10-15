#include "outputs.h"

#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdr.h"
#include "settings.h"
#include "vrxc_msp.h"

static const char *TAG = "out";

// Which stacks are up. Both can run at once: IDF's Wi-Fi/BT coexistence shares the
// 2.4 GHz radio, and a burst sends them one after the other anyway.
static bool bleOn = false;
static bool espnowOn = false;
static uint8_t activeUid[6];

// --- ESP-NOW -------------------------------------------------------------------

static void espnowStart(const uint8_t uid[6])
{
    // Like the ELRS backpack: our own MAC *is* the bind-phrase UID, and the
    // goggles/LapDog LCD accept frames from it.
    static bool netifReady = false;
    if (!netifReady)
    {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
        netifReady = true;
    }
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, uid));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 2.4 GHz only: 5 GHz ESP-NOW (ch 149-165) lands on the FPV video band.
    ESP_ERROR_CHECK(esp_wifi_set_channel(cfg::ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, uid, 6);
    p.channel = cfg::ESPNOW_CHANNEL;
    p.ifidx = WIFI_IF_STA;
    ESP_ERROR_CHECK(esp_now_add_peer(&p));
}

static void espnowStop()
{
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
}

static void espnowSend(const uint8_t *frame, size_t n)
{
    if (!n) return;
    if (esp_now_send(activeUid, frame, n) != ESP_OK) ESP_LOGW(TAG, "esp-now send failed");
    // Spaced like elrs-race-test: the backpack drops frames that arrive too close together.
    vTaskDelay(pdMS_TO_TICKS(cfg::ESPNOW_FRAME_GAP_MS));
}

// Clears one OSD row the way the timers do: a write of OSD_COLS NULs from column 0.
// (An empty write leaves the old text on HDZero.)
static size_t osdClearRow(uint8_t *out, size_t outLen, uint8_t row)
{
    uint8_t p[vrxc::OSD_TEXT_HEADER + vrxc::OSD_COLS] = {vrxc::OSD_TEXT, row, 0, 0};
    return vrxc::mspEncode(out, outLen, vrxc::MSP_ELRS_SET_OSD, p, sizeof(p));
}

// Text centred on the 50-column row, as the timers place it.
static size_t osdTextCentred(uint8_t *out, size_t outLen, uint8_t row, const char *text)
{
    int col = vrxc::OSD_COLS / 2 - (int)strlen(text) / 2;
    return vrxc::osdText(out, outLen, row, (uint8_t)(col < 0 ? 0 : col), text);
}

// --- BLE (NimBLE, non-connectable advert bursts) --------------------------------

static SemaphoreHandle_t bleSynced;

static void bleOnSync() { xSemaphoreGive(bleSynced); }

static void bleHostTask(void *)
{
    nimble_port_run(); // returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

static bool bleStart()
{
    if (!bleSynced) bleSynced = xSemaphoreCreateBinary();
    if (nimble_port_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble init failed");
        return false;
    }
    ble_hs_cfg.sync_cb = bleOnSync;
    nimble_port_freertos_init(bleHostTask);
    if (xSemaphoreTake(bleSynced, pdMS_TO_TICKS(2000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "nimble host didn't sync");
        return false;
    }
    ble_hs_util_ensure_addr(0);
    return true;
}

static void bleStop()
{
    ble_gap_adv_stop();
    if (nimble_port_stop() == 0) nimble_port_deinit();
}

static int bleGapEvent(struct ble_gap_event *, void *) { return 0; }

// Advertises one frame as manufacturer data for ms. Legacy advert, 31 bytes:
//   02 01 06                        flags
//   len FF <company LE> <frame>     manufacturer data (frame <= lapframe::MAX_SIZE)
static void bleAdvertise(const uint8_t *frame, size_t n, uint32_t ms)
{
    uint8_t adv[31];
    size_t o = 0;
    adv[o++] = 2;
    adv[o++] = BLE_HS_ADV_TYPE_FLAGS;
    adv[o++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv[o++] = (uint8_t)(1 + 2 + n);
    adv[o++] = BLE_HS_ADV_TYPE_MFG_DATA;
    adv[o++] = (uint8_t)(lapframe::BLE_COMPANY_ID & 0xFF);
    adv[o++] = (uint8_t)(lapframe::BLE_COMPANY_ID >> 8);
    memcpy(adv + o, frame, n);
    o += n;
    if (ble_gap_adv_set_data(adv, (int)o) != 0)
    {
        ESP_LOGW(TAG, "adv data rejected");
        return;
    }

    uint8_t ownAddr;
    ble_hs_id_infer_auto(0, &ownAddr);
    ble_gap_adv_params p = {};
    p.conn_mode = BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = p.itvl_max = BLE_GAP_ADV_ITVL_MS(cfg::BLE_ADV_INTERVAL_MS);
    int rc = ble_gap_adv_start(ownAddr, nullptr, BLE_HS_FOREVER, &p, bleGapEvent, nullptr);
    if (rc != 0)
    {
        // Pre-5.0 rules floor non-connectable adverts at 100 ms.
        p.itvl_min = p.itvl_max = BLE_GAP_ADV_ITVL_MS(100);
        rc = ble_gap_adv_start(ownAddr, nullptr, BLE_HS_FOREVER, &p, bleGapEvent, nullptr);
    }
    if (rc != 0)
    {
        ESP_LOGW(TAG, "adv start failed: %d", rc);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    ble_gap_adv_stop();
}

// --- Link selection -------------------------------------------------------------

void outputs::setLinks(uint8_t links, const uint8_t uid[6])
{
    bool wantBle = links & LINK_BLE;
    bool wantEspnow = links & LINK_ESPNOW;
    // A new bind phrase means a new MAC: restart ESP-NOW with it.
    if (espnowOn && (!wantEspnow || memcmp(uid, activeUid, 6)))
    {
        espnowStop();
        espnowOn = false;
    }
    if (bleOn && !wantBle)
    {
        bleStop();
        bleOn = false;
    }
    if (wantBle && !bleOn) bleOn = bleStart();
    if (wantEspnow && !espnowOn)
    {
        memcpy(activeUid, uid, 6);
        espnowStart(uid);
        espnowOn = true;
    }
    ESP_LOGI(TAG, "radio output: BLE %s, ESP-NOW %s", bleOn ? "on" : "off", espnowOn ? "on" : "off");
}

// --- Sink -----------------------------------------------------------------------

namespace
{

class DeviceSink : public race::Sink
{
public:
    void frame(const lapframe::Frame &f) override
    {
        uint8_t b[lapframe::MAX_SIZE];
        size_t n = lapframe::encode(f, b);
        printf("F:");
        for (size_t i = 0; i < n; i++) printf("%02X", b[i]);
        printf("\n");
        if (bleOn && frames_ < MAX_FRAMES)
        {
            memcpy(ble_[frames_], b, n);
            bleLen_[frames_++] = (uint8_t)n;
        }
    }

    void osd(uint8_t row, const char *text) override
    {
        if (!espnowOn || rows_ >= MAX_ROWS) return;
        rowNo_[rows_] = row;
        snprintf(rowText_[rows_++], sizeof(rowText_[0]), "%s", text);
    }

    void osdClear() override { clear_ = espnowOn; }

    void flush() override
    {
        if (!frames_ && !rows_ && !clear_) return;
        bool resume = sdr::running();
        if (resume) sdr::end();
        // ESP-NOW first: the goggles are what the pilot is looking at.
        if (rows_ || clear_) sendEspnow();
        if (frames_) sendBle();
        frames_ = rows_ = 0;
        clear_ = false;
        if (resume) sdr::begin(settings::get().freqMhz); // re-apply the PHY debug config
    }

private:
    static constexpr size_t MAX_FRAMES = 24;
    static constexpr size_t MAX_ROWS = 8;
    uint8_t ble_[MAX_FRAMES][lapframe::MAX_SIZE];
    uint8_t bleLen_[MAX_FRAMES];
    size_t frames_ = 0;
    uint8_t rowNo_[MAX_ROWS];
    char rowText_[MAX_ROWS][vrxc::OSD_COLS + 1];
    size_t rows_ = 0;
    bool clear_ = false;

    static bool keyFrame(const uint8_t *frame) { return (frame[0] & 0x0F) != lapframe::TEXT; }

    void sendBle()
    {
        // Key frames long, text short (see cfg::BLE_KEY_FRAME_MS). Key frames bar
        // heartbeats join the echo list, and the echo schedule starts again from now.
        bool added = false;
        for (size_t i = 0; i < frames_; i++)
        {
            bool key = keyFrame(ble_[i]);
            bleAdvertise(ble_[i], bleLen_[i], key ? cfg::BLE_KEY_FRAME_MS : cfg::BLE_TEXT_FRAME_MS);
            if (!key || (ble_[i][0] & 0x0F) == lapframe::HEARTBEAT) continue;
            if (echoFrames_ == MAX_ECHO) // full: the oldest goes
            {
                memmove(echo_[0], echo_[1], sizeof(echo_[0]) * (MAX_ECHO - 1));
                memmove(echoLen_, echoLen_ + 1, MAX_ECHO - 1);
                echoFrames_--;
            }
            memcpy(echo_[echoFrames_], ble_[i], bleLen_[i]);
            echoLen_[echoFrames_++] = bleLen_[i];
            added = true;
        }
        if (added)
        {
            echoFrom_ = (uint32_t)(esp_timer_get_time() / 1000);
            echoNext_ = 0;
        }
    }

public:
    // Echoes the key frames at each of cfg::BLE_ECHO_AFTER_MS, then forgets them.
    void tick(uint32_t nowMs)
    {
        if (!echoFrames_ || echoNext_ >= ECHOES) return;
        if (nowMs - echoFrom_ < cfg::BLE_ECHO_AFTER_MS[echoNext_]) return;
        echoNext_++;
        if (bleOn)
        {
            bool resume = sdr::running();
            if (resume) sdr::end();
            for (size_t i = 0; i < echoFrames_; i++) bleAdvertise(echo_[i], echoLen_[i], cfg::BLE_ECHO_FRAME_MS);
            if (resume) sdr::begin(settings::get().freqMhz);
        }
        if (echoNext_ >= ECHOES) echoFrames_ = 0;
    }

private:
    static constexpr size_t MAX_ECHO = 6;
    static constexpr size_t ECHOES = sizeof(cfg::BLE_ECHO_AFTER_MS) / sizeof(cfg::BLE_ECHO_AFTER_MS[0]);
    uint8_t echo_[MAX_ECHO][lapframe::MAX_SIZE];
    uint8_t echoLen_[MAX_ECHO];
    size_t echoFrames_ = 0;
    uint32_t echoFrom_ = 0; // ms: the burst the schedule counts from
    size_t echoNext_ = ECHOES;

    void sendEspnow()
    {
        uint8_t f[80];
        if (clear_) espnowSend(f, vrxc::osdClear(f, sizeof(f)));
        for (size_t i = 0; i < rows_; i++)
        {
            espnowSend(f, osdClearRow(f, sizeof(f), rowNo_[i])); // "" is just this: the row cleared
            if (rowText_[i][0]) espnowSend(f, osdTextCentred(f, sizeof(f), rowNo_[i], rowText_[i]));
        }
        espnowSend(f, vrxc::osdDisplay(f, sizeof(f)));
    }
};

DeviceSink deviceSink;

} // namespace

race::Sink &outputs::sink() { return deviceSink; }

void outputs::tick(uint32_t nowMs) { deviceSink.tick(nowMs); }
