#include "display.h"

#include "config.h"

#if LAPDOG_TOUCH

// The touch board's screens are in src/touch/.

#elif LAPDOG_LCD147

// The 1.47" board's screen is in src/lcd147/.

#elif !LAPDOG_OLED

void display::begin() {}
void display::frame(const lapframe::Frame &) {}
void display::splash() {}
void display::bootStep(const char *) {}
void display::bootDone() {}

#else

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "race.h"
#include "session_view.h"
#include "settings.h"

static const char *TAG = "oled";

// --- SSD1306 72x40 -----------------------------------------------------------------
// The 0.42" panel is the middle 72 columns of the controller's 128 (from 28)
// and 5 pages of 8 rows.

static constexpr int W = 72;
static constexpr int PAGES = 5;
static constexpr uint8_t COL_OFFSET = 28;
static constexpr uint8_t ADDR = 0x3C;

static i2c_master_dev_handle_t dev;
static uint8_t fb[PAGES][W];

static bool command(const uint8_t *c, size_t n)
{
    uint8_t b[32] = {0x00}; // control byte: commands follow
    memcpy(b + 1, c, n);
    return i2c_master_transmit(dev, b, n + 1, 50) == ESP_OK;
}

static bool panelInit()
{
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = cfg::PIN_OLED_SDA;
    bus.scl_io_num = cfg::PIN_OLED_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t h;
    if (i2c_new_master_bus(&bus, &h) != ESP_OK) return false;
    i2c_device_config_t d = {};
    d.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    d.device_address = ADDR;
    d.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(h, &d, &dev) != ESP_OK) return false;

    // As U8g2's SSD1306 72x40 ER init.
    static const uint8_t init[] = {
        0xAE,       // display off
        0xD5, 0x80, // clock
        0xA8, 0x27, // multiplex: 40 rows
        0xD3, 0x00, // display offset
        0x40,       // start line 0
        0x8D, 0x14, // charge pump on
        0x20, 0x02, // page addressing
        0xA1, 0xC8, // flipped both ways: right way up on this board
        0xDA, 0x12, // COM pins
        0x81, 0xFF, // contrast: full, it's read in daylight
        0xD9, 0xF1, // precharge
        0xDB, 0x40, // VCOM deselect
        0xAD, 0x30, // internal IREF (the ER panel needs it)
        0xA4, 0xA6, // show RAM, not inverted
        0xAF,       // display on
    };
    return command(init, sizeof(init));
}

static void flush()
{
    for (int p = 0; p < PAGES; p++)
    {
        uint8_t c[] = {(uint8_t)(0xB0 | p), (uint8_t)(COL_OFFSET & 0x0F), (uint8_t)(0x10 | (COL_OFFSET >> 4))};
        command(c, sizeof(c));
        uint8_t b[W + 1] = {0x40}; // control byte: data follows
        memcpy(b + 1, fb[p], W);
        i2c_master_transmit(dev, b, sizeof(b), 50);
    }
}

static void invert(bool on)
{
    uint8_t c = on ? 0xA7 : 0xA6;
    command(&c, 1);
}

// --- Drawing --------------------------------------------------------------------------
// Text sits on pages: small text is one page (12 characters across), big text
// is the same font doubled, two pages tall (6 across).

static const uint8_t *glyph(char ch)
{
    if (ch < font5x7::FIRST || ch > font5x7::LAST) ch = '?';
    return font5x7::GLYPHS[ch - font5x7::FIRST];
}

static void text(int x, int page, const char *s)
{
    for (; *s && x + 5 <= W; s++, x += 6) memcpy(&fb[page][x], glyph(*s), 5);
}

static void textRight(int page, const char *s) { text(W - 6 * (int)strlen(s) + 1, page, s); }

static void bigText(int x, int page, const char *s)
{
    for (; *s && x + 10 <= W; s++, x += 12)
    {
        const uint8_t *g = glyph(*s);
        for (int c = 0; c < 5; c++)
        {
            // Spread each of the column's 7 bits over two rows, across two pages.
            uint16_t col = 0;
            for (int bit = 0; bit < 8; bit++)
                if (g[c] & (1 << bit)) col |= 3u << (bit * 2);
            for (int dx = 0; dx < 2; dx++)
            {
                fb[page][x + c * 2 + dx] = (uint8_t)col;
                fb[page + 1][x + c * 2 + dx] = (uint8_t)(col >> 8);
            }
        }
    }
}

// --- State, from the session's frames ------------------------------------------------

using session_view::State;

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

void display::frame(const lapframe::Frame &f) { session_view::frame(f); }

static void draw(const State &s, uint32_t now)
{
    const Settings &set = settings::get();
    char line[24], t[16];
    memset(fb, 0, sizeof(fb));

    // Top line: what's running, and the channel it listens on.
    if (s.armed) snprintf(line, sizeof(line), "H%u %s", s.heat, s.mode == race::RACE ? "RACE" : "PRAC");
    else snprintf(line, sizeof(line), "LapDog");
    text(0, 0, line);
    if (set.chan[0]) textRight(0, set.chan);
    else
    {
        snprintf(t, sizeof(t), "%u", set.freqMhz);
        textRight(0, t);
    }

    if (!s.armed)
    {
        bigText(0, 1, "IDLE");
        if (s.everArmed && s.bestMs)
        {
            race::cmrcTime(s.bestMs, t, sizeof(t));
            snprintf(line, sizeof(line), "bst %s", t);
        }
        else snprintf(line, sizeof(line), "%u MHz", set.freqMhz);
        text(0, 3, line);
        bool ble = set.links & LINK_BLE, espnow = set.links & LINK_ESPNOW;
        text(0, 4, ble && espnow ? "BLE+ESPNOW" : ble ? "BLE" : espnow ? "ESPNOW" : "USB only");
        return;
    }

    if (s.stage == race::FINISHED)
    {
        bigText(0, 1, "DONE");
        if (s.bestMs)
        {
            race::cmrcTime(s.bestMs, t, sizeof(t));
            snprintf(line, sizeof(line), "bst %s", t);
            text(0, 3, line);
        }
        if (s.raceMs)
        {
            race::cmrcTime(s.raceMs, t, sizeof(t));
            snprintf(line, sizeof(line), "tot %s", t);
            text(0, 4, line);
        }
        return;
    }

    if (!s.started)
    {
        // Race: staging, then GO until the holeshot. Practice: waiting for the first pass.
        bigText(0, 1, s.stage == race::STAGING ? "READY" : s.stage == race::RACING ? "GO!" : "ARMED");
        text(0, 3, s.stage == race::STAGING ? "staging" : "fly the gate");
        return;
    }

    // The lap being flown, its running time and the last lap.
    if (s.lapLimit && s.mode == race::RACE) snprintf(line, sizeof(line), "L%u/%u", s.laps + 1, s.lapLimit);
    else snprintf(line, sizeof(line), "L%u", s.laps + 1);
    bigText(0, 1, line);
    race::cmrcTime(now - s.lapAtMs, t, sizeof(t));
    snprintf(line, sizeof(line), "cur %s", t);
    text(0, 3, line);
    if (s.laps)
    {
        race::cmrcTime(s.lastMs, t, sizeof(t));
        snprintf(line, sizeof(line), "lst %s%s", t, s.lastBest ? "*" : "");
        text(0, 4, line);
    }
}

static void task(void *)
{
    bool inverted = false;
    while (true)
    {
        uint32_t now = millis();
        State s = session_view::get();
        draw(s, now);
        flush();
        bool flash = (int32_t)(s.flashUntilMs - now) > 0;
        if (flash != inverted) invert(inverted = flash);
        vTaskDelay(pdMS_TO_TICKS(cfg::OLED_FRAME_MS));
    }
}

void display::begin()
{
    if (!panelInit())
    {
        ESP_LOGW(TAG, "no OLED found");
        return;
    }
    xTaskCreate(task, "oled", 3072, nullptr, 1, nullptr);
}

void display::splash() {}
void display::bootStep(const char *) {}
void display::bootDone() {}

#endif
