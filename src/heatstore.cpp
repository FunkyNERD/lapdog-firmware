#include "heatstore.h"

#if LAPDOG_TOUCH

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "heatstore";


static const char *STORE = "/store";
static const char *OUT = "/store/out";
static const char *SENT = "/store/sent";

// File layout: a small header, then the Heat as recorded. A new layout gets a new
// version; files of an older one are skipped (and left where they are).
static constexpr uint32_t MAGIC = 0x4C444854; // "THDL"
static constexpr uint16_t VERSION = 1;
struct Header
{
    uint32_t magic;
    uint16_t version;
    uint16_t size; // sizeof(heatlog::Heat)
    int32_t raceNo;
};

static bool mounted = false;
static heatstore::Usage cached = {}; // refreshed on the store's task after every change
static wl_handle_t wl = WL_INVALID_HANDLE;
static uint32_t nextId = 1;

// --- The store's task -----------------------------------------------------------------------
// Flash writes with the task's stack in PSRAM (cloud.cpp's) crash, and the timing
// loop's stack is too small for FAT: so every call is handed to this task, whose stack
// is in internal RAM, and the caller waits for it.

// The task only exists for the length of a call: internal RAM is scarce (the SDR's capture
// bank, the radios and the display's DMA buffers need what's left), so no stack is held
// back while idle. A call that can't get a stack (False) is treated as a failed call.
static TaskHandle_t storeTask = nullptr; // the running call's task, null between calls
static SemaphoreHandle_t callLock = nullptr;

struct Call
{
    void (*fn)(void *);
    void *arg;
    TaskHandle_t caller;
};

static void storeMain(void *p)
{
    Call *c = static_cast<Call *>(p);
    storeTask = xTaskGetCurrentTaskHandle();
    c->fn(c->arg);
    xTaskNotifyGive(c->caller);
    vTaskDelete(nullptr);
}

// Runs f on its own internal-stack task and waits. False (f not run) if there's no memory for one.
template <typename F> static bool onStore(F f)
{
    if (storeTask && xTaskGetCurrentTaskHandle() == storeTask)
    {
        f();
        return true;
    }
    if (!callLock) callLock = xSemaphoreCreateMutex();
    if (!callLock) return false;
    xSemaphoreTake(callLock, portMAX_DELAY);
    Call c = {[](void *a) { (*static_cast<F *>(a))(); }, &f, xTaskGetCurrentTaskHandle()};
    bool started = xTaskCreate(storeMain, "store", 5120, &c, 3, nullptr) == pdPASS;
    if (started) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    else ESP_LOGW(TAG, "no memory for the store task");
    storeTask = nullptr;
    xSemaphoreGive(callLock);
    return started;
}

// --- Files ----------------------------------------------------------------------------------

static void path(char *out, size_t len, const char *dir, uint32_t id)
{
    snprintf(out, len, "%s/H%05lu.BIN", dir, (unsigned long)id);
}

static bool idOf(const char *name, uint32_t &id)
{
    unsigned long n;
    char ext[4];
    if (sscanf(name, "H%5lu.%3s", &n, ext) != 2 || strcasecmp(ext, "BIN")) return false;
    id = (uint32_t)n;
    return true;
}

// Every heat number in a directory, ascending.
template <typename F> static void eachId(const char *dir, F f)
{
    DIR *d = opendir(dir);
    if (!d) return;
    while (struct dirent *e = readdir(d))
    {
        uint32_t id;
        if (idOf(e->d_name, id)) f(id);
    }
    closedir(d);
}

static int countIn(const char *dir)
{
    int n = 0;
    eachId(dir, [&](uint32_t) { n++; });
    return n;
}

static bool exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static bool readHeader(const char *p, Header &hd, FILE **keep = nullptr)
{
    FILE *f = fopen(p, keep ? "rb+" : "rb");
    if (!f) return false;
    bool ok = fread(&hd, sizeof(hd), 1, f) == 1 && hd.magic == MAGIC && hd.version == VERSION &&
              hd.size == sizeof(heatlog::Heat);
    if (keep && ok) *keep = f;
    else fclose(f);
    return ok;
}

// --- The flash volume -----------------------------------------------------------------------

// Counting the volume's free clusters and heats walks the FAT and the directories: done
// after a change, not on every screen refresh.
static void refreshUsage()
{
    heatstore::Usage u = {};
    esp_vfs_fat_info(STORE, &u.totalBytes, &u.freeBytes);
    u.waiting = countIn(OUT);
    u.sent = countIn(SENT);
    cached = u;
}

static bool beginImpl()
{
    if (mounted) return true;
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true; // the first boot after the partition appeared
    cfg.max_files = 4;
    cfg.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORE, "storage", &cfg, &wl);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "flash storage won't mount (%s): is the partition table flashed?", esp_err_to_name(err));
        return false;
    }
    mounted = true;
    mkdir(OUT, 0775);
    mkdir(SENT, 0775);
    uint32_t top = 0;
    auto high = [&](uint32_t id) { top = std::max(top, id); };
    eachId(OUT, high);
    eachId(SENT, high);
    nextId = top + 1;
    refreshUsage();
    ESP_LOGI(TAG, "flash storage %llu of %llu KB free; next heat file %lu, %d waiting", cached.freeBytes / 1024,
             cached.totalBytes / 1024, (unsigned long)nextId, cached.waiting);
    return true;
}

bool heatstore::begin()
{
    bool ok = false;
    onStore([&] { ok = beginImpl(); });
    return ok;
}

bool heatstore::ready() { return mounted; }

heatstore::Usage heatstore::usage() { return cached; }

bool heatstore::nearlyFull()
{
    Usage u = usage();
    return u.totalBytes && u.freeBytes * 10 < u.totalBytes;
}

static bool saveImpl(const heatlog::Heat &h)
{
    char p[40], tmp[40];
    uint32_t id = nextId++;
    path(p, sizeof(p), OUT, id);
    snprintf(tmp, sizeof(tmp), "%s/NEW.TMP", OUT);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    Header hd = {MAGIC, VERSION, (uint16_t)sizeof(heatlog::Heat), -1};
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1 && fwrite(&h, sizeof(h), 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    // Written whole, then renamed: a power cut never leaves half a heat behind.
    if (ok) ok = rename(tmp, p) == 0;
    if (!ok) unlink(tmp);
    ESP_LOGI(TAG, "heat %u saved as %lu%s", h.heat, (unsigned long)id, ok ? "" : " - FAILED (flash full?)");
    refreshUsage();
    return ok;
}

bool heatstore::save(const heatlog::Heat &h)
{
    if (!mounted || !heatlog::uploadable(h)) return false;
    bool ok = false;
    onStore([&] { ok = saveImpl(h); });
    return ok;
}

int heatstore::pending(Pending *out, int max)
{
    if (!mounted) return 0;
    int n = 0;
    onStore([&] {
        eachId(OUT, [&](uint32_t id) {
            if (n < max) out[n++] = {id, -1};
        });
        std::sort(out, out + n, [](const Pending &a, const Pending &b) { return a.id < b.id; });
        for (int i = 0; i < n; i++)
        {
            char p[40];
            path(p, sizeof(p), OUT, out[i].id);
            Header hd;
            out[i].raceNo = readHeader(p, hd) ? hd.raceNo : -1;
        }
    });
    return n;
}

int heatstore::pendingCount()
{
    return cached.waiting;
}

bool heatstore::load(uint32_t id, heatlog::Heat &h)
{
    bool ok = false;
    onStore([&] {
        char p[40];
        path(p, sizeof(p), OUT, id);
        FILE *f = nullptr;
        Header hd;
        if (!readHeader(p, hd, &f)) return;
        ok = fread(&h, sizeof(h), 1, f) == 1;
        fclose(f);
    });
    return ok;
}

static bool setRaceNoImpl(uint32_t id, int32_t raceNo)
{
    char p[40];
    path(p, sizeof(p), OUT, id);
    FILE *f = nullptr;
    Header hd;
    if (!readHeader(p, hd, &f)) return false;
    hd.raceNo = raceNo;
    bool ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(&hd, sizeof(hd), 1, f) == 1;
    return fclose(f) == 0 && ok;
}

bool heatstore::setRaceNo(uint32_t id, int32_t raceNo)
{
    bool ok = false;
    onStore([&] { ok = setRaceNoImpl(id, raceNo); });
    return ok;
}

bool heatstore::markSent(uint32_t id)
{
    bool ok = false;
    onStore([&] {
        char from[40], to[40];
        path(from, sizeof(from), OUT, id);
        path(to, sizeof(to), SENT, id);
        unlink(to);
        ok = rename(from, to) == 0;
        refreshUsage();
    });
    return ok;
}

void heatstore::clearRaceNos()
{
    if (!mounted) return;
    onStore([&] {
        std::vector<uint32_t> ids;
        eachId(OUT, [&](uint32_t id) { ids.push_back(id); });
        for (uint32_t id : ids) setRaceNoImpl(id, -1);
    });
}

void heatstore::resend()
{
    if (!mounted) return;
    onStore([&] {
        std::vector<uint32_t> ids;
        eachId(SENT, [&](uint32_t id) { ids.push_back(id); });
        for (uint32_t id : ids)
        {
            char from[40], to[40];
            path(from, sizeof(from), SENT, id);
            path(to, sizeof(to), OUT, id);
            rename(from, to);
        }
        refreshUsage();
    });
    clearRaceNos();
}

void heatstore::printInfo()
{
    Usage u = usage();
    if (!mounted) printf("S:store=none\n");
    else
        printf("S:store.kb=%llu\nS:store.free.kb=%llu\nS:store.waiting=%d\nS:store.sent=%d\nS:store.next=%lu\n",
               u.totalBytes / 1024, u.freeBytes / 1024, u.waiting, u.sent, (unsigned long)nextId);
}

#else

bool heatstore::begin() { return false; }
bool heatstore::ready() { return false; }
void heatstore::printInfo() {}
bool heatstore::save(const heatlog::Heat &) { return false; }
int heatstore::pending(Pending *, int) { return 0; }
int heatstore::pendingCount() { return 0; }
bool heatstore::load(uint32_t, heatlog::Heat &) { return false; }
bool heatstore::setRaceNo(uint32_t, int32_t) { return false; }
bool heatstore::markSent(uint32_t) { return false; }
void heatstore::clearRaceNos() {}
void heatstore::resend() {}
heatstore::Usage heatstore::usage() { return {}; }
bool heatstore::nearlyFull() { return false; }
#endif // LAPDOG_TOUCH
