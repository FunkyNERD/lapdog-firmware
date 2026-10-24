#include "heatstore.h"

#if LAPDOG_TOUCH

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "touch/board.h"

static const char *TAG = "heatstore";

static constexpr gpio_num_t PIN_SD_CS = GPIO_NUM_9;
static constexpr gpio_num_t PIN_SD_MISO = GPIO_NUM_2; // on the display's bus (board.cpp)
static constexpr spi_host_device_t SD_SPI = SPI2_HOST; // the display's bus (board.cpp)
static const char *MOUNT = "/sd";
static const char *OUT = "/sd/lapdog/out";
static const char *SENT = "/sd/lapdog/sent";

// File layout: a small header, then the Heat as recorded. A new layout gets a new
// version; files of an older one are skipped (and left on the card).
static constexpr uint32_t MAGIC = 0x4C444854; // "THDL"
static constexpr uint16_t VERSION = 1;
struct Header
{
    uint32_t magic;
    uint16_t version;
    uint16_t size; // sizeof(heatlog::Heat)
    int32_t raceNo;
};

static sdmmc_card_t *card = nullptr;

// Every card command waits for the bus: the display's DMA and the card's polled
// transfers can't share the SPI peripheral at once (Waveshare's firmware does the same).
static esp_err_t transaction(int slot, sdmmc_command_t *cmd)
{
    board::lockBus();
    esp_err_t err = sdspi_host_do_transaction(slot, cmd);
    board::unlockBus();
    return err;
}
static uint32_t nextId = 1;

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

bool heatstore::begin()
{
    // Deselect the card before anything else talks on the shared bus.
    gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_SD_CS, 1);
    gpio_pullup_en(PIN_SD_CS);
    gpio_pullup_en(PIN_SD_MISO); // the card's data out floats until it's in SPI mode

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI;
    host.do_transaction = transaction;
    host.max_freq_khz = 20000; // the bus also carries the display at 60 MHz, the card's limit is lower
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_SPI;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 3;
    mount.allocation_unit_size = 16 * 1024;
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &slot, &mount, &card);
    if (err != ESP_OK)
    {
        card = nullptr;
        ESP_LOGW(TAG, "no SD card (%s): check it's in and works in a computer (FAT32)", esp_err_to_name(err));
        return false;
    }
    mkdir("/sd/lapdog", 0775);
    mkdir(OUT, 0775);
    mkdir(SENT, 0775);
    uint32_t top = 0;
    auto high = [&](uint32_t id) { top = std::max(top, id); };
    eachId(OUT, high);
    eachId(SENT, high);
    nextId = top + 1;
    ESP_LOGI(TAG, "SD card %s, %llu MB; next heat file %lu, %d waiting", card->cid.name,
             ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024), (unsigned long)nextId,
             pendingCount());
    return true;
}

bool heatstore::ready() { return card != nullptr; }

void heatstore::printInfo()
{
    if (!card)
    {
        printf("S:sd=none\n");
        return;
    }
    int sent = 0;
    eachId(SENT, [&](uint32_t) { sent++; });
    printf("S:sd=%s\nS:sd.mb=%llu\nS:sd.waiting=%d\nS:sd.sent=%d\nS:sd.next=%lu\n", card->cid.name,
           ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024), pendingCount(), sent,
           (unsigned long)nextId);
}

bool heatstore::save(const heatlog::Heat &h)
{
    if (!card || !heatlog::uploadable(h)) return false;
    char p[40], tmp[40];
    uint32_t id = nextId++;
    path(p, sizeof(p), OUT, id);
    snprintf(tmp, sizeof(tmp), "%s/NEW.TMP", OUT);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    Header hd = {MAGIC, VERSION, (uint16_t)sizeof(heatlog::Heat), -1};
    bool ok = fwrite(&hd, sizeof(hd), 1, f) == 1 && fwrite(&h, sizeof(h), 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    // Written whole, then renamed: a pulled card never leaves half a heat behind.
    if (ok) ok = rename(tmp, p) == 0;
    if (!ok) unlink(tmp);
    ESP_LOGI(TAG, "heat %u saved as %lu%s", h.heat, (unsigned long)id, ok ? "" : " - FAILED");
    return ok;
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

int heatstore::pending(Pending *out, int max)
{
    if (!card) return 0;
    int n = 0;
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
    return n;
}

int heatstore::pendingCount()
{
    int n = 0;
    if (card) eachId(OUT, [&](uint32_t) { n++; });
    return n;
}

bool heatstore::load(uint32_t id, heatlog::Heat &h)
{
    char p[40];
    path(p, sizeof(p), OUT, id);
    FILE *f = nullptr;
    Header hd;
    if (!readHeader(p, hd, &f)) return false;
    bool ok = fread(&h, sizeof(h), 1, f) == 1;
    fclose(f);
    return ok;
}

bool heatstore::setRaceNo(uint32_t id, int32_t raceNo)
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

bool heatstore::markSent(uint32_t id)
{
    char from[40], to[40];
    path(from, sizeof(from), OUT, id);
    path(to, sizeof(to), SENT, id);
    unlink(to);
    return rename(from, to) == 0;
}

void heatstore::clearRaceNos()
{
    if (!card) return;
    std::vector<uint32_t> ids;
    eachId(OUT, [&](uint32_t id) { ids.push_back(id); });
    for (uint32_t id : ids) setRaceNo(id, -1);
}

void heatstore::resend()
{
    if (!card) return;
    std::vector<uint32_t> ids;
    eachId(SENT, [&](uint32_t id) { ids.push_back(id); });
    for (uint32_t id : ids)
    {
        char from[40], to[40];
        path(from, sizeof(from), SENT, id);
        path(to, sizeof(to), OUT, id);
        rename(from, to);
    }
    clearRaceNos();
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

#endif // LAPDOG_TOUCH
