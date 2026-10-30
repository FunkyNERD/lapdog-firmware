#include "display.h"

#if LAPDOG_LCD147

// Waveshare ESP32-C5-LCD-1.47: the 320x172 status screen. No touch and no PSRAM, so no
// LVGL: the screen is drawn a strip at a time from a short list of text items (the
// 5x7 font, scaled), into one small DMA buffer, and only when something changed.
// The SDR's capture bank takes 128 KB of the chip's internal RAM, which rules out a
// whole-frame buffer (110 KB) as well.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "race.h"
#include "session_view.h"
#include "settings.h"

static const char *TAG = "lcd147";

// --- Panel -----------------------------------------------------------------------------
// The ST7789 is 240x320 inside, with the 172-wide glass in the middle: 34 columns in from
// each side. Turned to landscape; if the picture comes out mirrored, upside down or
// shifted on a board, these are the knobs.
static constexpr int W = 320, H = 172;
static constexpr int STRIP = 22; // lines per transfer: 14 KB of DMA buffer
static constexpr uint32_t PCLK_HZ = 40 * 1000 * 1000;
static constexpr bool PANEL_SWAP_XY = true;
static constexpr bool PANEL_MIRROR_X = true;
static constexpr bool PANEL_MIRROR_Y = false;
static constexpr int GAP_X = 0, GAP_Y = 34;
static constexpr bool PANEL_INVERT = true;
static constexpr uint32_t FRAME_MS = 66; // redraw check; ~15 frames/s while the lap clock runs

static esp_lcd_panel_handle_t panel = nullptr;
static SemaphoreHandle_t txDone = nullptr;
static SemaphoreHandle_t drawing = nullptr; // one present() at a time
static uint16_t *strip = nullptr;
static volatile bool booting = true; // the splash has the screen until bootDone()

static bool onTxDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(txDone, &woken);
    return woken == pdTRUE;
}

static void backlight(int percent)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)(percent * 255 / 100));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static bool panelInit()
{
    // Backlight first, dark: the panel's memory is random until the first frame.
    ledc_timer_config_t tc = {};
    tc.speed_mode = LEDC_LOW_SPEED_MODE;
    tc.timer_num = LEDC_TIMER_0;
    tc.duty_resolution = LEDC_TIMER_8_BIT;
    tc.freq_hz = 5000;
    tc.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&tc);
    ledc_channel_config_t cc = {};
    cc.gpio_num = cfg::PIN_LCD_BL;
    cc.speed_mode = LEDC_LOW_SPEED_MODE;
    cc.channel = LEDC_CHANNEL_0;
    cc.timer_sel = LEDC_TIMER_0;
    cc.duty = 0;
    ledc_channel_config(&cc);

    spi_bus_config_t bus = {};
    bus.sclk_io_num = cfg::PIN_LCD_CLK;
    bus.mosi_io_num = cfg::PIN_LCD_MOSI;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = W * STRIP * sizeof(uint16_t);
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t ioCfg = {};
    ioCfg.dc_gpio_num = cfg::PIN_LCD_DC;
    ioCfg.cs_gpio_num = cfg::PIN_LCD_CS;
    ioCfg.pclk_hz = PCLK_HZ;
    ioCfg.lcd_cmd_bits = 8;
    ioCfg.lcd_param_bits = 8;
    ioCfg.spi_mode = 0;
    ioCfg.trans_queue_depth = 4;
    ioCfg.on_color_trans_done = onTxDone;
    txDone = xSemaphoreCreateBinary();
    drawing = xSemaphoreCreateMutex();
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioCfg, &io) != ESP_OK) return false;

    esp_lcd_panel_dev_config_t pc = {};
    pc.reset_gpio_num = cfg::PIN_LCD_RST;
    pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    pc.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(io, &pc, &panel) != ESP_OK) return false;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, PANEL_INVERT);
    esp_lcd_panel_swap_xy(panel, PANEL_SWAP_XY);
    esp_lcd_panel_mirror(panel, PANEL_MIRROR_X, PANEL_MIRROR_Y);
    esp_lcd_panel_set_gap(panel, GAP_X, GAP_Y);

    strip = (uint16_t *)heap_caps_malloc(W * STRIP * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    return strip != nullptr;
}

// --- Drawing ---------------------------------------------------------------------------

// RGB565, byte-swapped for the SPI (the panel wants the high byte first).
static constexpr uint16_t c565(uint32_t rgb)
{
    uint16_t v = (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    return (uint16_t)((v << 8) | (v >> 8));
}

namespace col // the touch screens' palette (src/touch/theme.h)
{
constexpr uint16_t BG = c565(0x0A0C0F), LINE = c565(0x262B33), TEXT = c565(0xF3F5F7), SOFT = c565(0xC9CFD6),
                   MUTED = c565(0xA0A8B3), ACCENT = c565(0x3D9BE9), BEST = c565(0xC4A1FF), OK = c565(0x3DD68C),
                   OK_TINT = c565(0x0F2A1E);
}

enum Align : uint8_t { LEFT, RIGHT, CENTER };

struct Item
{
    int16_t x, y;
    uint8_t scale, align;
    uint16_t color;
    char s[24];
};

// Everything on the screen. Built fresh each frame and compared with the last: if it's
// the same, nothing is sent. (Zeroed first, so the compare sees no stray bytes.)
struct Scene
{
    uint16_t bg;
    int16_t bar;  // signal bar fill, 0-100 %, or -1 for none
    int16_t tick; // where the detector's enter threshold sits on it, in %
    bool border;  // splash: the panel's edges, to check the alignment
    uint8_t n;
    Item items[10];
};

struct Band // the strip being drawn: lines y0 .. y0+h-1 of the screen
{
    int y0, h;
};

static void fill(const Band &b, int x, int y, int w, int h, uint16_t c)
{
    int xa = x < 0 ? 0 : x, xb = x + w > W ? W : x + w;
    int ya = y < b.y0 ? b.y0 : y, yb = y + h > b.y0 + b.h ? b.y0 + b.h : y + h;
    for (int yy = ya; yy < yb; yy++)
    {
        uint16_t *row = strip + (yy - b.y0) * W;
        for (int xx = xa; xx < xb; xx++) row[xx] = c;
    }
}

static int textWidth(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? n * 6 * scale - scale : 0;
}

static void drawText(const Band &b, const Item &it)
{
    if (it.y + 7 * it.scale <= b.y0 || it.y >= b.y0 + b.h) return;
    int x = it.x;
    if (it.align == RIGHT) x -= textWidth(it.s, it.scale);
    else if (it.align == CENTER) x -= textWidth(it.s, it.scale) / 2;
    for (const char *p = it.s; *p; p++, x += 6 * it.scale)
    {
        char ch = (*p < font5x7::FIRST || *p > font5x7::LAST) ? '?' : *p;
        const uint8_t *g = font5x7::GLYPHS[ch - font5x7::FIRST];
        for (int c = 0; c < 5; c++)
            for (int r = 0; r < 7; r++)
                if (g[c] & (1 << r)) fill(b, x + c * it.scale, it.y + r * it.scale, it.scale, it.scale, it.color);
    }
}

static void render(const Band &b, const Scene &sc)
{
    fill(b, 0, b.y0, W, b.h, sc.bg);
    if (sc.border)
    {
        fill(b, 0, 0, W, 1, col::TEXT);
        fill(b, 0, H - 1, W, 1, col::TEXT);
        fill(b, 0, 0, 1, H, col::TEXT);
        fill(b, W - 1, 0, 1, H, col::TEXT);
    }
    else
        fill(b, 8, 26, W - 16, 1, col::LINE);
    for (int i = 0; i < sc.n; i++) drawText(b, sc.items[i]);
    if (sc.bar >= 0)
    {
        const int x = 8, w = W - 16, y = 166, h = 5;
        fill(b, x, y, w, h, col::LINE);
        fill(b, x, y, w * sc.bar / 100, h, sc.bar >= sc.tick ? col::OK : col::ACCENT);
        fill(b, x + w * sc.tick / 100, y - 2, 2, h + 4, col::TEXT);
    }
}

static void present(const Scene &sc)
{
    if (!panel || !strip) return;
    xSemaphoreTake(drawing, portMAX_DELAY);
    xSemaphoreTake(txDone, 0);
    for (int y = 0; y < H; y += STRIP)
    {
        Band b = {y, H - y < STRIP ? H - y : STRIP};
        render(b, sc);
        if (esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + b.h, strip) != ESP_OK) break;
        if (xSemaphoreTake(txDone, pdMS_TO_TICKS(100)) != pdTRUE) ESP_LOGW(TAG, "transfer lost");
    }
    xSemaphoreGive(drawing);
}

static void add(Scene &sc, int x, int y, int scale, Align a, uint16_t color, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));

static void add(Scene &sc, int x, int y, int scale, Align a, uint16_t color, const char *fmt, ...)
{
    if (sc.n >= sizeof(sc.items) / sizeof(sc.items[0])) return;
    Item &it = sc.items[sc.n++];
    it.x = (int16_t)x;
    it.y = (int16_t)y;
    it.scale = (uint8_t)scale;
    it.align = a;
    it.color = color;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it.s, sizeof(it.s), fmt, ap);
    va_end(ap);
}

// --- The screens -----------------------------------------------------------------------

static void compose(Scene &sc, const session_view::State &s, uint32_t now)
{
    memset(&sc, 0, sizeof(sc));
    sc.bg = col::BG;
    sc.bar = -1;
    const Settings &set = settings::get();
    char t[16];

    // Top line: what's running, and the channel it listens on.
    if (s.armed) add(sc, 8, 6, 2, LEFT, col::SOFT, "H%u %s", s.heat, s.mode == race::RACE ? "RACE" : "PRACTICE");
    else add(sc, 8, 6, 2, LEFT, col::SOFT, "LAPDOG");
    if (set.chan[0]) add(sc, W - 8, 6, 2, RIGHT, col::MUTED, "%s %u", set.chan, set.freqMhz);
    else add(sc, W - 8, 6, 2, RIGHT, col::MUTED, "%u MHZ", set.freqMhz);

    if (!s.armed)
    {
        add(sc, W / 2, 40, 7, CENTER, col::TEXT, "IDLE");
        if (s.everArmed && s.bestMs)
        {
            race::cmrcTime(s.bestMs, t, sizeof(t));
            add(sc, W / 2, 104, 3, CENTER, col::BEST, "BEST %s", t);
        }
        bool ble = set.links & LINK_BLE, espnow = set.links & LINK_ESPNOW;
        add(sc, W / 2, 134, 2, CENTER, col::MUTED, "%s", ble && espnow ? "BLE + ESP-NOW" : ble ? "BLE" : espnow ? "ESP-NOW" : "USB ONLY");
        add(sc, W / 2, 152, 2, CENTER, col::MUTED, "HOLD BOOT TO ARM");
        return;
    }

    if ((int32_t)(s.flashUntilMs - now) > 0) sc.bg = col::OK_TINT;
    sc.bar = (int16_t)(app::signalDb() / 30.0f * 100);
    if (sc.bar < 0) sc.bar = 0;
    if (sc.bar > 100) sc.bar = 100;
    sc.tick = (int16_t)(set.detector.enterDb / 30.0f * 100);
    if (sc.tick > 100) sc.tick = 100;

    if (s.stage == race::FINISHED)
    {
        add(sc, W / 2, 40, 7, CENTER, col::TEXT, "DONE");
        if (s.bestMs)
        {
            race::cmrcTime(s.bestMs, t, sizeof(t));
            add(sc, W / 2, 104, 3, CENTER, col::BEST, "BEST %s", t);
        }
        if (s.raceMs)
        {
            race::cmrcTime(s.raceMs, t, sizeof(t));
            add(sc, W / 2, 134, 3, CENTER, col::SOFT, "TOTAL %s", t);
        }
        return;
    }

    if (!s.started)
    {
        // Race: staging, then GO until the holeshot. Practice: waiting for the first pass.
        add(sc, W / 2, 48, 7, CENTER, col::TEXT, "%s", s.stage == race::STAGING ? "READY" : s.stage == race::RACING ? "GO!" : "ARMED");
        add(sc, W / 2, 118, 3, CENTER, col::MUTED, "%s", s.stage == race::STAGING ? "STAGING" : "FLY THE GATE");
        return;
    }

    // The lap being flown, its running time, the last lap and the best.
    if (s.lapLimit && s.mode == race::RACE) add(sc, 8, 32, 3, LEFT, col::SOFT, "LAP %u/%u", s.laps + 1, s.lapLimit);
    else add(sc, 8, 32, 3, LEFT, col::SOFT, "LAP %u", s.laps + 1);
    race::cmrcTime(now - s.lapAtMs, t, sizeof(t));
    add(sc, W / 2, 58, 7, CENTER, col::TEXT, "%s", t);
    if (s.laps)
    {
        race::cmrcTime(s.lastMs, t, sizeof(t));
        add(sc, 8, 114, 3, LEFT, s.lastBest ? col::BEST : col::SOFT, "LAST %s", t);
        race::cmrcTime(s.bestMs, t, sizeof(t));
        add(sc, 8, 140, 3, LEFT, col::BEST, "BEST %s", t);
    }
}

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

static void task(void *)
{
    // On the heap: static data sits below the SDR's capture bank, and there's no room left there.
    Scene *shown = (Scene *)calloc(1, sizeof(Scene)), *next = (Scene *)calloc(1, sizeof(Scene));
    if (!shown || !next) vTaskDelete(nullptr);
    bool first = true;
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
        if (booting) continue;
        compose(*next, session_view::get(), millis());
        if (!first && !memcmp(shown, next, sizeof(Scene))) continue;
        first = false;
        present(*next);
        memcpy(shown, next, sizeof(Scene));
    }
}

// --- Boot splash -----------------------------------------------------------------------

static const char *bootLabel = "";

static void drawSplash()
{
    Scene sc = {};
    sc.bg = col::BG;
    sc.bar = -1;
    add(sc, W / 2, 34, 9, CENTER, col::TEXT, "LAPDOG");
    add(sc, W / 2, 106, 3, CENTER, col::MUTED, "V" FIRMWARE_VERSION);
    add(sc, W / 2, 144, 2, CENTER, col::SOFT, "%s", bootLabel);
    present(sc);
}

void display::splash()
{
    if (!panelInit())
    {
        ESP_LOGE(TAG, "panel did not start");
        return;
    }
    esp_lcd_panel_disp_on_off(panel, true);
    drawSplash();
    backlight(100);
}

void display::bootStep(const char *label)
{
    bootLabel = label;
    drawSplash();
}

void display::bootDone() { booting = false; }

void display::begin()
{
    if (!panel) return;
    xTaskCreate(task, "screen", 4096, nullptr, 1, nullptr);
}

void display::frame(const lapframe::Frame &f) { session_view::frame(f); }

#endif // LAPDOG_LCD147
