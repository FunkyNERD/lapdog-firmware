#include "cloud.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "config.h"
#include "heat_log.h"
#include "heatstore.h"

#if LAPDOG_TOUCH

#include <new>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "cloud";

using namespace cloud;

static const char *NVS_NS = "cloud";
static constexpr int CONNECT_TIMEOUT_MS = 15000;
static constexpr int HTTP_TIMEOUT_MS = 15000;
static constexpr size_t BODY_MAX = 4096; // responses and race uploads both fit
static constexpr int MAX_PENDING = 64;   // heats per sync; the next sync sends the rest

// --- State ------------------------------------------------------------------------------

// What's kept in NVS. The task changes it under the lock and sets dirty; the timing
// loop writes it (tick()).
struct Prefs
{
    char token[80];
    char device[65];
    char pilot[65];
    int32_t epoch; // the website's numbering epoch the next number belongs to; -1 = none yet
    uint32_t next; // the next race number to give a heat
    uint8_t networkCount;
    Network networks[MAX_NETWORKS];
};

// In PSRAM (begin()): static data here would grow into the SDR's capture bank.
struct State
{
    Prefs prefs;
    Prefs saving; // tick()'s copy
    ScanResult scans[MAX_SCAN];
};
static State *state = nullptr;
static bool prefsDirty = false;
static SemaphoreHandle_t lock = nullptr;
static Status jobStatus = {};
static int scanN = 0;
static volatile int pendingN = -1;
static volatile bool radioBusy = false;

static TaskHandle_t worker = nullptr;
static QueueHandle_t heats = nullptr; // finished heats, timing loop -> task
static volatile Job wanted = JOB_NONE;
static volatile bool sdWanted = false; // the console's "sd": try the card again
static char pairCode[16];

static heatlog::Recorder *recorder = nullptr; // timing loop only; in PSRAM

// The task's big buffers, in PSRAM.
struct Work
{
    heatstore::Pending list[MAX_PENDING];
    heatlog::Heat heat;   // being uploaded
    heatlog::Heat saving; // off the queue, to the card
    char body[BODY_MAX];  // a race upload
};
static Work *work = nullptr;
static uint32_t bootId = 0;

static EventGroupHandle_t wifiEvents = nullptr;
static constexpr EventBits_t GOT_IP = BIT0, DISCONNECTED = BIT1;

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// Internal RAM is what runs out (the SDR's bank takes 128 KB of it).
static void logHeap(const char *where)
{
    ESP_LOGI(TAG, "%s: internal heap %u free, largest %u, lowest %u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

struct Lock
{
    Lock() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(lock); }
};

static void setStatus(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void setStatus(const char *fmt, ...)
{
    Lock l;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(jobStatus.message, sizeof(jobStatus.message), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", jobStatus.message);
}

static void setProgress(uint16_t done, uint16_t total)
{
    Lock l;
    jobStatus.done = done;
    jobStatus.total = total;
}

static void changePrefs(void (*f)(Prefs &, const void *), const void *arg)
{
    Lock l;
    f(state->prefs, arg);
    prefsDirty = true;
}

static void loadPrefs()
{
    nvs_handle_t h;
    state->prefs.epoch = -1;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(Prefs);
    if (nvs_get_blob(h, "prefs", &state->saving, &len) == ESP_OK && len == sizeof(Prefs)) state->prefs = state->saving;
    nvs_close(h);
}

void cloud::tick()
{
    if (!lock || !prefsDirty || xSemaphoreTake(lock, 0) != pdTRUE) return;
    Prefs &copy = state->saving;
    copy = state->prefs;
    prefsDirty = false;
    xSemaphoreGive(lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "prefs", &copy, sizeof(copy)) != ESP_OK || nvs_commit(h) != ESP_OK)
        ESP_LOGW(TAG, "saving the pairing failed");
    nvs_close(h);
}

bool cloud::busy() { return radioBusy; }

bool cloud::paired()
{
    if (!state) return false;
    Lock l;
    return state->prefs.token[0] != '\0';
}

const char *cloud::deviceName() { return state ? state->prefs.device : ""; }
const char *cloud::pilotName() { return state ? state->prefs.pilot : ""; }
bool cloud::sdReady() { return heatstore::ready(); }
int cloud::pending() { return pendingN; }

int cloud::networkCount()
{
    if (!state) return 0;
    Lock l;
    return state->prefs.networkCount;
}

cloud::Network cloud::network(int i)
{
    if (!state) return {};
    Lock l;
    return i >= 0 && i < state->prefs.networkCount ? state->prefs.networks[i] : Network{};
}

bool cloud::saveNetwork(const char *ssid, const char *pass)
{
    Network n = {};
    snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    snprintf(n.pass, sizeof(n.pass), "%s", pass ? pass : "");
    if (!state) return false;
    Lock l;
    int at = -1;
    for (int i = 0; i < state->prefs.networkCount; i++)
        if (!strcmp(state->prefs.networks[i].ssid, n.ssid)) at = i;
    if (at < 0)
    {
        if (state->prefs.networkCount >= MAX_NETWORKS) return false;
        at = state->prefs.networkCount++;
    }
    state->prefs.networks[at] = n;
    prefsDirty = true;
    return true;
}

void cloud::removeNetwork(int i)
{
    if (!state) return;
    Lock l;
    if (i < 0 || i >= state->prefs.networkCount) return;
    for (int j = i; j + 1 < state->prefs.networkCount; j++) state->prefs.networks[j] = state->prefs.networks[j + 1];
    state->prefs.networkCount--;
    prefsDirty = true;
}

cloud::Status cloud::status()
{
    if (!state) return {};
    Lock l;
    return jobStatus;
}

int cloud::scanCount()
{
    if (!state) return 0;
    Lock l;
    return scanN;
}

cloud::ScanResult cloud::scanResult(int i)
{
    if (!state) return {};
    Lock l;
    return i >= 0 && i < scanN ? state->scans[i] : ScanResult{};
}

// --- Heats from the timing loop -----------------------------------------------------------

static int64_t clockNow()
{
    time_t t = time(nullptr);
    return t >= 1577836800 ? (int64_t)t : 0; // before 2020: never set since power-on
}

void cloud::armContext(const char *event, const char *prefix)
{
    if (recorder) recorder->context(event, prefix, clockNow(), bootId, millis());
}

void cloud::frame(const lapframe::Frame &f)
{
    if (recorder && recorder->frame(f) && heatlog::uploadable(recorder->done()))
    {
        if (xQueueSend(heats, &recorder->done(), 0) != pdTRUE) ESP_LOGW(TAG, "heat %u dropped: queue full", f.heat);
        else if (worker) xTaskNotifyGive(worker);
    }
}

// --- Radio --------------------------------------------------------------------------------

static void onWifi(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifiEvents, GOT_IP);
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "wi-fi disconnected, reason %u", ((wifi_event_sta_disconnected_t *)data)->reason);
        xEventGroupSetBits(wifiEvents, DISCONNECTED);
    }
}

static wifi_band_mode_t savedBand = WIFI_BAND_MODE_AUTO;

// Takes the radio from the SDR/ESP-NOW for a job. False if the timer got armed first.
static bool borrowRadio()
{
    radioBusy = true;
    vTaskDelay(pdMS_TO_TICKS(2 * cfg::SAMPLE_PERIOD_MS)); // the timing loop sees busy() before we start
    if (app::armed())
    {
        radioBusy = false;
        setStatus("DISARM THE TIMER FIRST");
        return false;
    }
    logHeap("radio");
    esp_wifi_set_promiscuous(false);
    esp_wifi_get_band_mode(&savedBand);
    esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    return true;
}

static void returnRadio()
{
    esp_wifi_disconnect();
    esp_wifi_set_band_mode(savedBand);
    // As sdr.cpp left it: promiscuous keeps the receive path running for the SDR.
    if (cfg::HAS_SDR) esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(cfg::ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    radioBusy = false;
}

static int scanNetworks()
{
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    uint16_t n = 24;
    wifi_ap_record_t *recs = (wifi_ap_record_t *)heap_caps_malloc(n * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    if (!recs) return -1;
    esp_wifi_scan_get_ap_records(&n, recs);
    Lock l;
    scanN = 0;
    for (int i = 0; i < n && scanN < MAX_SCAN; i++)
    {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0] || recs[i].primary > 14) continue; // hidden, or 5 GHz
        bool dup = false;
        for (int j = 0; j < scanN; j++) dup |= !strcmp(state->scans[j].ssid, ssid); // another access point of it
        if (dup) continue;
        ScanResult &r = state->scans[scanN++];
        snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
        r.rssi = recs[i].rssi;
        r.open = recs[i].authmode == WIFI_AUTH_OPEN;
    }
    heap_caps_free(recs);
    // Strongest first (records come sorted, but dedup keeps the first anyway).
    return scanN;
}

// Joins the strongest saved network in range.
static bool connect()
{
    int nets;
    Network saved[MAX_NETWORKS];
    {
        Lock l;
        nets = state->prefs.networkCount;
        memcpy(saved, state->prefs.networks, sizeof(saved));
    }
    if (!nets)
    {
        setStatus("ADD A WI-FI NETWORK FIRST");
        return false;
    }
    setStatus("LOOKING FOR WI-FI...");
    scanNetworks();
    for (int pass = 0; pass < 2; pass++)
    {
        // First the saved networks the scan heard, strongest first; then the rest (hidden ones).
        for (int si = 0; si < (pass ? nets : scanCount()); si++)
        {
            const Network *n = nullptr;
            if (pass == 0)
            {
                ScanResult r = cloud::scanResult(si);
                for (int i = 0; i < nets; i++)
                    if (!strcmp(saved[i].ssid, r.ssid)) n = &saved[i];
            }
            else
            {
                bool heard = false;
                for (int i = 0; i < scanCount(); i++) heard |= !strcmp(cloud::scanResult(i).ssid, saved[si].ssid);
                if (!heard) n = &saved[si];
            }
            if (!n) continue;
            setStatus("JOINING %s...", n->ssid);
            wifi_config_t wc = {};
            memcpy(wc.sta.ssid, n->ssid, strlen(n->ssid));
            memcpy(wc.sta.password, n->pass, strlen(n->pass));
            wc.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
            wc.sta.pmf_cfg.capable = true;
            xEventGroupClearBits(wifiEvents, GOT_IP | DISCONNECTED);
            if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) continue;
            EventBits_t b = xEventGroupWaitBits(wifiEvents, GOT_IP | DISCONNECTED, pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
            if (b & GOT_IP) return true;
            esp_wifi_disconnect();
        }
    }
    setStatus("CAN'T JOIN A SAVED WI-FI NETWORK");
    return false;
}

// --- HTTPS --------------------------------------------------------------------------------

struct Response
{
    int status; // <= 0: couldn't reach the website
    char *body; // NUL-terminated, in the shared buffer
};

static char *buffer = nullptr; // BODY_MAX, PSRAM: requests and responses take turns

static Response request(const char *method, const char *path, const char *json)
{
    char url[128];
    snprintf(url, sizeof(url), "%s%s", cfg::WEBSITE_URL, path);
    esp_http_client_config_t c = {};
    c.url = url;
    c.timeout_ms = HTTP_TIMEOUT_MS;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.method = !strcmp(method, "POST") ? HTTP_METHOD_POST : !strcmp(method, "PUT") ? HTTP_METHOD_PUT : HTTP_METHOD_GET;
    c.buffer_size = 1024;
    c.buffer_size_tx = 1024;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    Response r = {-1, buffer};
    buffer[0] = '\0';
    if (!h) return r;
    esp_http_client_set_header(h, "Accept", "application/json");
    esp_http_client_set_header(h, "X-Firmware-Version", FIRMWARE_VERSION);
    char auth[96];
    {
        Lock l;
        snprintf(auth, sizeof(auth), "Bearer %s", state->prefs.token);
    }
    if (state->prefs.token[0]) esp_http_client_set_header(h, "Authorization", auth);
    int len = json ? (int)strlen(json) : 0;
    if (json) esp_http_client_set_header(h, "Content-Type", "application/json");
    if (esp_http_client_open(h, len) == ESP_OK)
    {
        if (len) esp_http_client_write(h, json, len);
        esp_http_client_fetch_headers(h);
        r.status = esp_http_client_get_status_code(h);
        int n = 0, got;
        while (n < (int)BODY_MAX - 1 && (got = esp_http_client_read(h, buffer + n, BODY_MAX - 1 - n)) > 0) n += got;
        buffer[n] = '\0';
    }
    esp_http_client_cleanup(h);
    return r;
}

// The website's message ({"error", "message"}), else a generic one.
static void describeError(const Response &r, const char *what)
{
    if (r.status <= 0)
    {
        setStatus("%s FAILED - CAN'T REACH THE WEBSITE", what);
        return;
    }
    cJSON *doc = cJSON_Parse(r.body);
    const cJSON *m = cJSON_GetObjectItem(doc, "message");
    if (cJSON_IsString(m)) setStatus("%s", m->valuestring);
    else setStatus("%s FAILED (HTTP %d)", what, r.status);
    cJSON_Delete(doc);
}

static void forget(Prefs &p, const void *)
{
    p.token[0] = p.device[0] = p.pilot[0] = '\0';
    p.epoch = -1;
}

// A 401: unpaired on the website.
static bool lostPairing(const Response &r)
{
    if (r.status != 401) return false;
    changePrefs(forget, nullptr);
    setStatus("UNPAIRED ON THE WEBSITE - PAIR AGAIN");
    return true;
}

static const char *str(const cJSON *o, const char *a, const char *b = nullptr)
{
    const cJSON *v = cJSON_GetObjectItem(o, a);
    if (b) v = cJSON_GetObjectItem(v, b);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

// Names, clock and numbering from a /pair or /device response.
struct Counters
{
    bool have;
    uint32_t race;
    int32_t epoch;
};

static Counters checkIn(const char *body)
{
    Counters c = {};
    cJSON *doc = cJSON_Parse(body);
    if (!doc) return c;
    const cJSON *t = cJSON_GetObjectItem(doc, "server_time");
    if (cJSON_IsNumber(t) && t->valuedouble > 1577836800)
    {
        timeval tv = {(time_t)t->valuedouble, 0};
        settimeofday(&tv, nullptr);
    }
    const char *dev = str(doc, "device", "name"), *pilot = str(doc, "user", "name");
    {
        Lock l;
        if (dev) snprintf(state->prefs.device, sizeof(state->prefs.device), "%s", dev);
        if (pilot) snprintf(state->prefs.pilot, sizeof(state->prefs.pilot), "%s", pilot);
        prefsDirty = true;
    }
    const cJSON *cs = cJSON_GetObjectItem(doc, "counters");
    const cJSON *race = cJSON_GetObjectItem(cs, "race"), *epoch = cJSON_GetObjectItem(cs, "epoch");
    if (cJSON_IsNumber(race))
        c = {true, (uint32_t)race->valuedouble, cJSON_IsNumber(epoch) ? (int32_t)epoch->valuedouble : 0};
    cJSON_Delete(doc);
    return c;
}

// --- Jobs ---------------------------------------------------------------------------------

static bool jobScan()
{
    setStatus("SCANNING FOR WI-FI...");
    int n = scanNetworks();
    if (n < 0)
    {
        setStatus("SCAN FAILED");
        return false;
    }
    setStatus(n ? "%d NETWORKS FOUND" : "NO 2.4 GHZ NETWORKS FOUND", n);
    return true;
}

// Every heat not uploaded yet, oldest first, numbered from the website's counter as
// LapDog LCD and the app do (device-api.md, "counters").
static bool uploadHeats(const Counters &server)
{
    // Internal RAM is kept for the SDR's capture bank: the big buffers are in PSRAM (begin()).
    heatstore::Pending *list = work->list;
    int n = heatstore::pending(list, MAX_PENDING);
    pendingN = heatstore::pendingCount();
    if (!n)
    {
        setStatus("SYNCED - NOTHING NEW TO UPLOAD");
        return true;
    }
    int32_t epoch;
    uint32_t next;
    {
        Lock l;
        epoch = state->prefs.epoch;
        next = state->prefs.next;
    }
    if (server.have)
    {
        if (epoch == server.epoch) next = next > server.race ? next : server.race;
        else
        {
            // A reset on the website (or the first sync): its numbers, and fresh ones for what's waiting.
            next = server.race;
            heatstore::clearRaceNos();
            for (int i = 0; i < n; i++) list[i].raceNo = -1;
            epoch = server.epoch;
        }
    }
    for (int i = 0; i < n; i++)
        if (list[i].raceNo < 0 && heatstore::setRaceNo(list[i].id, (int32_t)next)) list[i].raceNo = (int32_t)next++;
    {
        Lock l;
        state->prefs.epoch = epoch;
        state->prefs.next = next;
        prefsDirty = true;
    }

    heatlog::Heat &h = work->heat;
    char *body = work->body;
    int sent = 0;
    for (int i = 0; i < n; i++)
    {
        setProgress((uint16_t)i, (uint16_t)n);
        setStatus("UPLOADING HEAT %d OF %d...", i + 1, n);
        if (list[i].raceNo < 0 || !heatstore::load(list[i].id, h)) continue;
        // A heat armed before the clock was set this boot can still be dated now.
        int64_t at = h.startedAt;
        int64_t now = clockNow();
        if (!at && now && h.bootId == bootId) at = now - (int64_t)(millis() - h.upMs) / 1000;
        if (!heatlog::uploadJson(h, at, body, BODY_MAX)) continue;
        char path[40];
        snprintf(path, sizeof(path), "/api/v1/races/%ld", (long)list[i].raceNo);
        Response r = request("PUT", path, body);
        // 422: the website will never take it, so don't send it every sync.
        if (r.status == 200 || r.status == 201 || r.status == 422)
        {
            heatstore::markSent(list[i].id);
            sent++;
            continue;
        }
        if (!lostPairing(r)) describeError(r, "UPLOAD");
        pendingN = heatstore::pendingCount();
        return false;
    }
    setProgress((uint16_t)n, (uint16_t)n);
    pendingN = heatstore::pendingCount();
    if (pendingN > 0) setStatus("UPLOADED %d HEAT%s - SYNC AGAIN FOR THE REST", sent, sent == 1 ? "" : "S");
    else setStatus("UPLOADED %d HEAT%s", sent, sent == 1 ? "" : "S");
    return true;
}

static bool jobSync()
{
    if (!connect()) return false;
    setStatus("CHECKING IN WITH THE WEBSITE...");
    Response r = request("GET", "/api/v1/device", nullptr);
    if (r.status != 200)
    {
        if (!lostPairing(r)) describeError(r, "SYNC");
        return false;
    }
    Counters c = checkIn(r.body);
    return uploadHeats(c);
}

static bool jobPair()
{
    if (!connect()) return false;
    setStatus("PAIRING...");
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "code", pairCode);
    cJSON_AddStringToObject(doc, "name", "LapDog touch timer");
    cJSON_AddStringToObject(doc, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(doc, "kind", "timer");
    char *json = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    changePrefs(forget, nullptr); // a fresh pairing: no token on the request
    Response r = request("POST", "/api/v1/pair", json);
    cJSON_free(json);
    if (r.status != 201)
    {
        describeError(r, "PAIRING");
        return false;
    }
    cJSON *res = cJSON_Parse(r.body);
    const char *token = str(res, "token");
    if (!token)
    {
        cJSON_Delete(res);
        setStatus("PAIRING FAILED - UNEXPECTED REPLY");
        return false;
    }
    {
        Lock l;
        snprintf(state->prefs.token, sizeof(state->prefs.token), "%s", token);
        state->prefs.epoch = -1;
        prefsDirty = true;
    }
    cJSON_Delete(res);
    Counters c = checkIn(r.body);
    // Maybe another pilot's account: it gets every heat on the card.
    heatstore::resend();
    setStatus("PAIRED AS %s", state->prefs.device);
    return uploadHeats(c);
}

static bool jobCheck()
{
    if (!connect()) return false;
    setStatus("CHECKING THE WEBSITE...");
    Response r = request("GET", "/healthz", nullptr);
    if (r.status == 200) setStatus("WEBSITE OK - HTTPS WORKS");
    else describeError(r, "CHECK");
    return r.status == 200;
}

static bool jobUnpair()
{
    // Best effort: the pilot can always unpair it on the website too.
    if (connect()) request("POST", "/api/v1/unpair", nullptr);
    changePrefs(forget, nullptr);
    setStatus("UNPAIRED");
    return true;
}

static void task(void *)
{
    if (!heatstore::begin()) setStatus("NO SD CARD - HEATS AREN'T KEPT FOR SYNC");
    pendingN = heatstore::pendingCount();
    heatlog::Heat &h = work->saving;
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // A card put in (or switched on) after boot: try again whenever it's needed.
        if (!heatstore::ready() && (sdWanted || uxQueueMessagesWaiting(heats) || wanted != JOB_NONE))
        {
            if (heatstore::begin()) setStatus("SD CARD READY");
            pendingN = heatstore::pendingCount();
        }
        if (sdWanted) heatstore::printInfo();
        sdWanted = false;
        while (xQueueReceive(heats, &h, 0) == pdTRUE)
        {
            heatstore::save(h);
            pendingN = heatstore::pendingCount();
        }
        Job job = wanted;
        if (job == JOB_NONE) continue;
        wanted = JOB_NONE;
        bool ok = false;
        if (borrowRadio())
        {
            switch (job)
            {
            case JOB_SCAN: ok = jobScan(); break;
            case JOB_PAIR: ok = jobPair(); break;
            case JOB_SYNC: ok = jobSync(); break;
            case JOB_UNPAIR: ok = jobUnpair(); break;
            case JOB_CHECK: ok = jobCheck(); break;
            default: break;
            }
            returnRadio();
        }
        Lock l;
        jobStatus.running = false;
        jobStatus.ok = ok;
    }
}

static bool start(Job j)
{
    if (!state || !worker) return false;
    Lock l;
    if (jobStatus.running)
    {
        snprintf(jobStatus.message, sizeof(jobStatus.message), "BUSY - WAIT FOR THE LAST ONE");
        return false;
    }
    if (app::armed())
    {
        snprintf(jobStatus.message, sizeof(jobStatus.message), "DISARM THE TIMER FIRST");
        return false;
    }
    jobStatus = {j, true, false, "", 0, 0};
    wanted = j;
    xTaskNotifyGive(worker);
    return true;
}

bool cloud::startScan() { return start(JOB_SCAN); }

void cloud::checkSd()
{
    sdWanted = true;
    if (worker) xTaskNotifyGive(worker);
}

bool cloud::startPair(const char *code)
{
    snprintf(pairCode, sizeof(pairCode), "%s", code);
    return start(JOB_PAIR);
}

bool cloud::startSync()
{
    if (!paired())
    {
        Lock l;
        snprintf(jobStatus.message, sizeof(jobStatus.message), "PAIR WITH THE WEBSITE FIRST");
        return false;
    }
    return start(JOB_SYNC);
}

bool cloud::startUnpair() { return paired() && start(JOB_UNPAIR); }
bool cloud::startCheck() { return start(JOB_CHECK); }

void cloud::begin()
{
    state = (State *)heap_caps_calloc(1, sizeof(State), MALLOC_CAP_SPIRAM);
    if (!state)
    {
        ESP_LOGE(TAG, "no memory for website sync");
        return;
    }
    lock = xSemaphoreCreateMutex();
    loadPrefs();
    bootId = esp_random();
    void *mem = heap_caps_malloc(sizeof(heatlog::Recorder), MALLOC_CAP_SPIRAM);
    if (mem) recorder = new (mem) heatlog::Recorder();
    buffer = (char *)heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);
    work = (Work *)heap_caps_calloc(1, sizeof(Work), MALLOC_CAP_SPIRAM);
    heats = xQueueCreateWithCaps(2, sizeof(heatlog::Heat), MALLOC_CAP_SPIRAM);
    wifiEvents = xEventGroupCreate();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifi, nullptr);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, onWifi, nullptr);
    if (!recorder || !buffer || !work || !heats ||
        xTaskCreateWithCaps(task, "cloud", 10 * 1024, nullptr, 2, &worker, MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "no memory for website sync");
    ESP_LOGI(TAG, "website sync: %s, %d networks saved", state->prefs.token[0] ? state->prefs.device : "not paired",
             state->prefs.networkCount);
}

#else

void cloud::begin() {}
void cloud::frame(const lapframe::Frame &) {}
void cloud::armContext(const char *, const char *) {}
void cloud::tick() {}
bool cloud::busy() { return false; }
bool cloud::paired() { return false; }
const char *cloud::deviceName() { return ""; }
const char *cloud::pilotName() { return ""; }
bool cloud::sdReady() { return false; }
int cloud::pending() { return -1; }
int cloud::networkCount() { return 0; }
cloud::Network cloud::network(int) { return {}; }
bool cloud::saveNetwork(const char *, const char *) { return false; }
void cloud::removeNetwork(int) {}
bool cloud::startScan() { return false; }
void cloud::checkSd() {}
bool cloud::startPair(const char *) { return false; }
bool cloud::startSync() { return false; }
bool cloud::startUnpair() { return false; }
bool cloud::startCheck() { return false; }
cloud::Status cloud::status() { return {}; }
int cloud::scanCount() { return 0; }
cloud::ScanResult cloud::scanResult(int) { return {}; }

#endif // LAPDOG_TOUCH
