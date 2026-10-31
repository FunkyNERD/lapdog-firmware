#include "display.h"

#if LAPDOG_LCD147

// Waveshare ESP32-C5-LCD-1.47: the 320x172 timer screen, after the "LapDog 1.47" timer
// display" design. It's the touch timer's look (src/touch/) at this size: the same
// palette, the lockup, a channel chip, Last/Best cards with the best in purple, a solid
// blue ARM bar. There's no touch, so BOOT does it all and the screens say so.
//
// No LVGL: there's no PSRAM, and the SDR's capture bank takes 128 KB of internal RAM, so
// not even a whole frame buffer (110 KB) fits. The screen is drawn a strip at a time into
// one small DMA buffer, straight from the session's state, and only when something
// changed. Text is bitmap fonts (art.h, from tools/gen-lcd147-art.py); the rest is
// rectangles with anti-aliased corners.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "art.h"
#include "config.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
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
static uint16_t *strip = nullptr;

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

// --- Drawing primitives ----------------------------------------------------------------
// Everything draws into the strip being built: lines bandY .. bandY+bandH-1 of the screen.
// Colours are RGB565 as the CPU holds them; the strip is byte-swapped on its way out.

static int bandY = 0, bandH = 0;

static constexpr uint16_t c565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

namespace col // the touch screens' palette (src/touch/theme.h) and the app's HUD
{
constexpr uint16_t BG = c565(0x0A0C0F), SURFACE = c565(0x14171C), LINE = c565(0x262B33), TEXT = c565(0xF3F5F7),
                   SOFT = c565(0xC9CFD6), MUTED = c565(0xA0A8B3), CHEVRON = c565(0x6B7480), ACCENT = c565(0x3D9BE9),
                   ON_ACCENT = c565(0x06121F), SELECTED = c565(0x0E1C2A), SELECTED_TEXT = c565(0x8CC4F5),
                   BEST = c565(0xC4A1FF), BEST_TINT = c565(0x1C1729), BEST_EDGE = c565(0x2C2342),
                   SLOW = c565(0xFF8A8A), SLOW_BG = c565(0x2A1416), SLOW_EDGE = c565(0x4A2226), OK = c565(0x3DD68C),
                   OK_TINT = c565(0x0F2A1E), IDLE_DOT = c565(0x8A93A0), IDLE_RING = c565(0x1B1F26),
                   ARC_OFF = c565(0x242C3A), TRACK = c565(0x20262F);
}

static inline uint16_t mix(uint16_t dst, uint16_t src, unsigned a) // a: 0-255 of src
{
    if (a >= 255) return src;
    if (a == 0) return dst;
    unsigned ia = 255 - a;
    unsigned r = (((src >> 11) & 31) * a + ((dst >> 11) & 31) * ia + 127) / 255;
    unsigned g = (((src >> 5) & 63) * a + ((dst >> 5) & 63) * ia + 127) / 255;
    unsigned b = ((src & 31) * a + (dst & 31) * ia + 127) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline void put(int x, int y, uint16_t c, unsigned a)
{
    if (x < 0 || x >= W || y < bandY || y >= bandY + bandH) return;
    uint16_t *p = strip + (y - bandY) * W + x;
    *p = mix(*p, c, a);
}

static void fillRect(int x, int y, int w, int h, uint16_t c)
{
    int xa = x < 0 ? 0 : x, xb = x + w > W ? W : x + w;
    int ya = y < bandY ? bandY : y, yb = y + h > bandY + bandH ? bandY + bandH : y + h;
    for (int yy = ya; yy < yb; yy++)
    {
        uint16_t *row = strip + (yy - bandY) * W;
        for (int xx = xa; xx < xb; xx++) row[xx] = c;
    }
}

// A rectangle with anti-aliased round corners.
static void rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    int xa = x < 0 ? 0 : x, xb = x + w > W ? W : x + w;
    int ya = y < bandY ? bandY : y, yb = y + h > bandY + bandH ? bandY + bandH : y + h;
    for (int yy = ya; yy < yb; yy++)
    {
        uint16_t *row = strip + (yy - bandY) * W;
        bool top = yy < y + r, bottom = yy >= y + h - r;
        float cy = top ? (float)(y + r) : (float)(y + h - r);
        for (int xx = xa; xx < xb; xx++)
        {
            unsigned a = 255;
            if ((top || bottom) && (xx < x + r || xx >= x + w - r))
            {
                float cx = xx < x + r ? (float)(x + r) : (float)(x + w - r);
                float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy;
                float d = sqrtf(dx * dx + dy * dy);
                if (d >= r + 0.5f) continue;
                if (d > r - 0.5f) a = (unsigned)((r + 0.5f - d) * 255.0f);
            }
            row[xx] = mix(row[xx], c, a);
        }
    }
}

// A card: a 1 px border round a filled body.
static void card(int x, int y, int w, int h, int r, uint16_t bg, uint16_t edge)
{
    rrect(x, y, w, h, r, edge);
    rrect(x + 1, y + 1, w - 2, h - 2, r - 1, bg);
}

static void triangle(int x, int y, int w, int h, uint16_t c) // pointing right
{
    for (int yy = y; yy < y + h; yy++)
    {
        float t = 1.0f - fabsf((yy + 0.5f - (y + h / 2.0f)) / (h / 2.0f));
        int span = (int)(w * t + 0.5f);
        if (span > 0) fillRect(x, yy, span, 1, c);
    }
}

// An image of RGB565 pixels and an alpha plane.
static void blit(const uint16_t *c, const uint8_t *a, int w, int h, int x, int y)
{
    for (int yy = 0; yy < h; yy++)
    {
        int sy = y + yy;
        if (sy < bandY || sy >= bandY + bandH) continue;
        for (int xx = 0; xx < w; xx++)
        {
            unsigned al = a[yy * w + xx];
            if (al) put(x + xx, sy, c[yy * w + xx], al);
        }
    }
}

// A signal arc: part of a ring about (cx, cy), +-48 degrees about straight right, 5 px thick
// with round ends, lit between ARC_OFF and ACCENT.
static void arc(float cx, float cy, float radius, unsigned level)
{
    const float half = 2.5f, slope = 1.1106f; // tan(48 deg)
    const float ex = radius * 0.6691f, ey = radius * 0.7431f; // an end of the arc
    uint16_t color = mix(col::ARC_OFF, col::ACCENT, level);
    int y0 = (int)(cy - radius - half - 1), y1 = (int)(cy + radius + half + 2);
    for (int yy = y0; yy < y1; yy++)
    {
        if (yy < bandY || yy >= bandY + bandH) continue;
        for (int xx = (int)(cx - 4); xx < (int)(cx + radius + half + 3); xx++)
        {
            float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy;
            float r = sqrtf(dx * dx + dy * dy);
            if (fabsf(r - radius) > half + 4.0f) continue;
            float d;
            if (dx > 0 && fabsf(dy) <= slope * dx) d = fabsf(r - radius);
            else
            {
                float ty = dy < 0 ? -ey : ey;
                float a = dx - ex, b = dy - ty;
                d = sqrtf(a * a + b * b);
            }
            float cov = half + 0.5f - d;
            if (cov <= 0) continue;
            put(xx, yy, color, cov >= 1.0f ? 255u : (unsigned)(cov * 255.0f));
        }
    }
}

// --- Text ------------------------------------------------------------------------------

enum Align : uint8_t { LEFT, RIGHT, CENTER };

static const art::Glyph *glyphFor(const art::Font &f, uint16_t code)
{
    int lo = 0, hi = (int)f.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (f.glyphs[mid].code == code) return &f.glyphs[mid];
        if (f.glyphs[mid].code < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

// The next character of a UTF-8 string, as a code point: ASCII and the middle dot, else '?'.
static uint16_t nextChar(const char *&p)
{
    unsigned char c = (unsigned char)*p++;
    if (c < 0x80) return c;
    if (c == 0xC2 && (unsigned char)*p == 0xB7)
    {
        p++;
        return 0xB7;
    }
    while (((unsigned char)*p & 0xC0) == 0x80) p++;
    return '?';
}

static const art::Glyph *glyphOrBlank(const art::Font &f, uint16_t code)
{
    const art::Glyph *g = glyphFor(f, code);
    return g ? g : glyphFor(f, '?');
}

// Width in 1/16 px; track16 is the letter spacing in 1/16 px.
static int width16(const art::Font &f, const char *s, int track16)
{
    int w = 0, n = 0;
    while (*s)
    {
        const art::Glyph *g = glyphOrBlank(f, nextChar(s));
        if (g) w += g->advance;
        n++;
    }
    return n ? w + track16 * (n - 1) : 0;
}

static int textWidth(const art::Font &f, const char *s, int track16 = 0) { return (width16(f, s, track16) + 8) / 16; }

static void drawGlyph(const art::Font &f, const art::Glyph &g, int x, int baseline, uint16_t color)
{
    int gx = x + g.x, gy = baseline + g.y;
    for (int row = 0; row < g.h; row++)
    {
        int y = gy + row;
        if (y < bandY || y >= bandY + bandH) continue;
        for (int colm = 0; colm < g.w; colm++)
        {
            unsigned i = (unsigned)row * g.w + colm;
            uint8_t b = f.alpha[g.off + i / 2];
            unsigned a = (i & 1) ? (b & 15) : (b >> 4);
            if (a) put(gx + colm, y, color, a * 17);
        }
    }
}

// A line of text with its line box top at `top`, `lh` tall (as CSS centres it): `x` is the
// left edge, the right edge or the middle, by `align`.
static void text(const art::Font &f, int x, int top, int lh, const char *s, uint16_t color, Align align = LEFT,
                 int track16 = 0)
{
    int baseline = top + (lh - (f.ascent + f.descent)) / 2 + f.ascent;
    if (baseline - f.ascent >= bandY + bandH || baseline + f.descent <= bandY) return;
    int w16 = width16(f, s, track16);
    int pen = x * 16;
    if (align == RIGHT) pen -= w16;
    else if (align == CENTER) pen -= w16 / 2;
    while (*s)
    {
        const art::Glyph *g = glyphOrBlank(f, nextChar(s));
        if (!g) continue;
        if (g->w) drawGlyph(f, *g, (pen + 8) / 16, baseline, color);
        pen += g->advance + track16;
    }
}

// Shortens s (in place, in a buffer of n) with "..." until it fits `maxw` px.
static void fitText(char *s, size_t n, const art::Font &f, int maxw, int track16 = 0)
{
    (void)n;
    size_t len = strlen(s);
    if (textWidth(f, s, track16) <= maxw) return;
    while (len > 1)
    {
        len--;
        s[len] = '\0';
        char t[64];
        snprintf(t, sizeof(t), "%s...", s);
        if (textWidth(f, t, track16) <= maxw)
        {
            strcat(s, "...");
            return;
        }
    }
}

// --- What's on the screen --------------------------------------------------------------
// The whole screen, as plain values. Built fresh each frame from the session and compared
// with the last: if it's the same, nothing is sent. (Zeroed first, so the comparison sees no
// stray bytes.)

enum Kind : uint8_t { K_SPLASH, K_IDLE, K_WAIT, K_LIVE, K_FINISHED };
enum Tone : uint8_t { T_NORMAL, T_BEST, T_SLOW }; // card and held-lap colouring

struct Card
{
    char label[20];
    char value[12];
    char delta[10];
    uint8_t tone;      // T_NORMAL or T_BEST
    uint8_t deltaTone; // 0 none, 1 faster (green), 2 slower (red)
};

struct Scene
{
    uint8_t kind;
    uint8_t dot;               // armed (green) or finished (grey)
    uint8_t hint;              // 0 hold: stop; 1 tap: next heat, hold: stop
    uint8_t held, heldTone;    // a finished lap on show, and how it went
    uint8_t pct, tick, hot;    // signal bar: level, where the enter threshold sits (percent), above it
    uint8_t arcs[3];           // splash: how lit each arc is
    uint8_t progress;          // splash: percent
    bool haveLast;
    char chip[24], radio[20], status[28], heat[20];
    char event[48], heatBig[28], mode[12], sub[44];
    char lapWord[16], big[24], clock[12], lapNo[6], lapTotal[6], tag[12], lapDone[16], label[28];
    char lastTitle[16], lastLaps[12];
    Card a, b;
};

// --- The screens: drawing ---------------------------------------------------------------

static void chipRight(int right, int top, const char *s, const art::Font &f, uint16_t color, int track16)
{
    int w = textWidth(f, s, track16) + 18;
    card(right - w, top, w, 22, 11, col::SURFACE, col::LINE);
    text(f, right - w / 2, top, 22, s, color, CENTER, track16);
}

static void statusRow(const Scene &sc)
{
    bool armed = sc.dot == 0;
    rrect(9, 10, 14, 14, 7, armed ? col::OK_TINT : col::IDLE_RING);
    rrect(12, 13, 8, 8, 4, armed ? col::OK : col::IDLE_DOT);
    text(art::PLEX12B, 28, 6, 22, sc.status, col::TEXT, LEFT, 12);
    int x = 28 + textWidth(art::PLEX12B, sc.status, 12) + 8;
    text(art::PLEX12, x, 6, 22, sc.heat, col::MUTED);
    chipRight(308, 6, sc.chip, art::MONO12, col::SELECTED_TEXT, 0);
}

static void footer(const Scene &sc)
{
    text(art::PLEX11B, 12, 148, 16, "SIGNAL", col::MUTED, LEFT, 14);
    int bx = 12 + textWidth(art::PLEX11B, "SIGNAL", 14) + 7, bw = 128 - bx;
    rrect(bx, 153, bw, 6, 3, col::SURFACE);
    int fw = bw * sc.pct / 100;
    if (fw > 0) rrect(bx, 153, fw < 6 ? 6 : fw, 6, 3, sc.hot ? col::OK : col::SOFT);
    fillRect(bx + bw * sc.tick / 100, 150, 2, 12, col::TEXT);

    const char *hint = sc.hint ? "tap: next heat \xC2\xB7 hold: stop" : "hold: stop";
    int tx = 308 - textWidth(art::PLEX11, hint);
    text(art::PLEX11, 308, 147, 18, hint, col::MUTED, RIGHT);
    int kw = textWidth(art::MONO10, "BOOT") + 12;
    int kx = tx - 6 - kw;
    card(kx, 148, kw, 16, 4, col::BG, col::LINE);
    text(art::MONO10, kx + kw / 2, 148, 16, "BOOT", col::SOFT, CENTER);
}

static void wordmark(int x, int top, const art::Font &f, int track16)
{
    text(f, x, top, 26, "LAP", col::TEXT, LEFT, track16);
    int w = textWidth(f, "LAP", track16) + (track16 + 8) / 16;
    text(f, x + w, top, 26, "DOG", col::ACCENT, LEFT, track16);
}

static void drawIdle(const Scene &sc)
{
    blit(art::mark_c, art::mark_a, art::MARK_W, art::MARK_H, 12, 7);
    wordmark(45, 6, art::CHAKRA16, 16);
    int right = 308;
    int w = textWidth(art::PLEX11B, sc.radio, 12) + 18;
    card(right - w, 7, w, 22, 11, col::SURFACE, col::LINE);
    text(art::PLEX11B, right - w / 2, 7, 22, sc.radio, col::SOFT, CENTER, 12);
    chipRight(right - w - 6, 7, sc.chip, art::MONO12, col::SELECTED_TEXT, 0);

    if (sc.event[0]) text(art::PLEX12, 12, 42, 14, sc.event, col::MUTED);
    text(art::CHAKRA46, 12, 58, 50, sc.heatBig, col::TEXT);
    int mw = textWidth(art::PLEX11B, sc.mode, 14) + 18;
    rrect(12, 114, mw, 20, 6, col::SELECTED);
    text(art::PLEX11B, 12 + mw / 2, 114, 20, sc.mode, col::SELECTED_TEXT, CENTER, 14);
    text(art::PLEX12, 12 + mw + 8, 114, 20, sc.sub, col::MUTED);

    card(190, 44, 118, 86, 10, col::SURFACE, col::LINE);
    text(art::PLEX12, 200, 50, 14, sc.lastTitle, col::MUTED);
    text(art::PLEX12, 298, 50, 14, sc.lastLaps, col::MUTED, RIGHT);
    text(art::PLEX11B, 200, 69, 10, "BEST", col::MUTED, LEFT, 14);
    text(art::MONO21, 200, 81, 24, sc.a.value, sc.haveLast ? col::BEST : col::MUTED);
    text(art::PLEX11B, 200, 108, 15, "AVG", col::MUTED, LEFT, 14);
    text(art::MONO15, 298, 108, 15, sc.b.value, sc.haveLast ? col::TEXT : col::MUTED, RIGHT);

    rrect(12, 136, 296, 30, 10, col::ACCENT);
    const char *arm = "HOLD BOOT TO ARM";
    int aw = textWidth(art::CHAKRA15, arm, 19);
    int ax = 160 - (12 + 9 + aw) / 2;
    triangle(ax, 144, 12, 14, col::ON_ACCENT);
    text(art::CHAKRA15, ax + 21, 136, 30, arm, col::ON_ACCENT, LEFT, 19);
}

static void drawWait(const Scene &sc)
{
    statusRow(sc);
    text(art::PLEX12B, 160, 44, 14, sc.lapWord, col::MUTED, CENTER, 28);
    text(art::CHAKRA54, 160, 60, 58, sc.big, col::TEXT, CENTER, 35);
    text(art::PLEX14, 160, 122, 16, sc.sub, col::MUTED, CENTER);
    footer(sc);
}

static void drawCard(int x, const Card &c)
{
    bool best = c.tone == T_BEST;
    card(x, 98, 144, 44, 10, best ? col::BEST_TINT : col::SURFACE, best ? col::BEST_EDGE : col::LINE);
    text(art::PLEX11, x + 10, 104, 13, c.label, best ? col::BEST : col::MUTED);
    if (c.deltaTone) text(art::MONO12, x + 134, 104, 13, c.delta, c.deltaTone == 1 ? col::OK : col::SLOW, RIGHT);
    text(art::MONO21, x + 10, 117, 24, c.value, best ? col::BEST : col::TEXT);
}

static void drawLive(const Scene &sc)
{
    statusRow(sc);
    uint16_t clockColor = col::TEXT;
    if (sc.held)
    {
        bool best = sc.heldTone == T_BEST;
        clockColor = best ? col::BEST : sc.heldTone == T_SLOW ? col::SLOW : col::TEXT;
        if (sc.heldTone != T_NORMAL)
            card(6, 30, 308, 64, 12, best ? col::BEST_TINT : col::SLOW_BG, best ? col::BEST_EDGE : col::SLOW_EDGE);
        text(art::MONO56, 14, 32, 60, sc.clock, clockColor, LEFT, -18);
        if (sc.tag[0])
        {
            const art::Font &f = best ? art::CHAKRA13 : art::MONO15;
            int tw = textWidth(f, sc.tag, best ? 16 : 0) + 20;
            rrect(304 - tw, 38, tw, 22, 11, best ? col::BEST_EDGE : col::SLOW_EDGE);
            text(f, 304 - tw / 2, 38, 22, sc.tag, clockColor, CENTER, best ? 16 : 0);
        }
        text(art::PLEX11B, 304, 66, 13, sc.lapDone, col::MUTED, RIGHT, 25);
    }
    else
    {
        text(art::MONO56, 10, 32, 60, sc.clock, clockColor, LEFT, -18);
        text(art::PLEX11B, 308, 36, 13, sc.lapWord, col::MUTED, RIGHT, 25);
        int tw = sc.lapTotal[0] ? textWidth(art::CHAKRA16, sc.lapTotal) : 0;
        if (tw) text(art::CHAKRA16, 308, 49, 44, sc.lapTotal, col::MUTED, RIGHT);
        text(art::CHAKRA40, 308 - tw, 49, 44, sc.lapNo, col::TEXT, RIGHT);
    }
    drawCard(12, sc.a);
    drawCard(164, sc.b);
    footer(sc);
}

static void drawSplash(const Scene &sc)
{
    // The dog's bitmap is at 1 px per design unit; its origin is the brand kit's mark box.
    const int mx = 111, my = 9;
    blit(art::dog_c, art::dog_a, art::DOG_W, art::DOG_H, mx, my);
    const float cx = mx + (100 - art::MARK_X), cy = my + (56 - art::MARK_Y);
    arc(cx, cy, 11, sc.arcs[0]);
    arc(cx, cy, 19, sc.arcs[1]);
    arc(cx, cy, 27, sc.arcs[2]);
    wordmark(160 - (textWidth(art::CHAKRA26, "LAPDOG", 48) + 3) / 2, 98, art::CHAKRA26, 48);
    rrect(60, 136, 200, 4, 2, col::TRACK);
    if (sc.progress) rrect(60, 136, 200 * sc.progress / 100 < 4 ? 4 : 200 * sc.progress / 100, 4, 2, col::ACCENT);
    text(art::PLEX11B, 160, 146, 14, sc.label, col::MUTED, CENTER, 18);
    text(art::MONO10, 310, 154, 12, "v" FIRMWARE_VERSION, col::CHEVRON, RIGHT);
}

static void render(const Scene &sc)
{
    fillRect(0, bandY, W, bandH, col::BG);
    switch (sc.kind)
    {
    case K_SPLASH: drawSplash(sc); break;
    case K_IDLE: drawIdle(sc); break;
    case K_WAIT: drawWait(sc); break;
    case K_LIVE:
    case K_FINISHED: drawLive(sc); break;
    }
}

static void present(const Scene &sc)
{
    if (!panel || !strip) return;
    xSemaphoreTake(txDone, 0);
    for (int y = 0; y < H; y += STRIP)
    {
        bandY = y;
        bandH = H - y < STRIP ? H - y : STRIP;
        render(sc);
        uint16_t *p = strip;
        for (int i = 0; i < W * bandH; i++) p[i] = (uint16_t)((p[i] << 8) | (p[i] >> 8)); // the panel wants the high byte first
        if (esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + bandH, strip) != ESP_OK) break;
        if (xSemaphoreTake(txDone, pdMS_TO_TICKS(100)) != pdTRUE) ESP_LOGW(TAG, "transfer lost");
    }
}

// --- The screens: what they say ----------------------------------------------------------

static void fmt(char *out, size_t n, const char *f, ...) __attribute__((format(printf, 3, 4)));
static void fmt(char *out, size_t n, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    vsnprintf(out, n, f, ap);
    va_end(ap);
}

// A lap time: "21.30", or "1:21.30" from a minute up.
static void lapTime(uint32_t ms, char *out, size_t n) { race::cmrcTime(ms, out, n); }

// The clock, always with minutes: "0:23.45".
static void clockTime(uint32_t ms, char *out, size_t n)
{
    unsigned long cs = ms / 10;
    if (cs >= 60000) cs = 59999;
    snprintf(out, n, "%lu:%02lu.%02lu", cs / 6000, cs / 100 % 60, cs % 100);
}

static void deltaTime(int32_t ms, char *out, size_t n)
{
    unsigned long a = (unsigned long)(ms < 0 ? -ms : ms) / 10;
    if (a > 9999) a = 9999;
    snprintf(out, n, "%c%lu.%02lu", ms < 0 ? '-' : '+', a / 100, a % 100);
}

static void upper(const char *in, char *out, size_t n)
{
    size_t i = 0;
    for (; in[i] && i + 1 < n; i++) out[i] = (in[i] >= 'a' && in[i] <= 'z') ? (char)(in[i] - 32) : in[i];
    out[i] = '\0';
}

static void limitText(const Settings &set, char *out, size_t n)
{
    if (set.raceLimit == race::LIMIT_LAPS) fmt(out, n, "%u laps", set.raceLaps);
    else fmt(out, n, "%u:%02u", set.raceTimeS / 60, set.raceTimeS % 60);
}

static void signalBar(Scene &sc, const Settings &set)
{
    float db = app::signalDb();
    int pct = (int)(db / 30.0f * 100);
    int tick = (int)(set.detector.enterDb / 30.0f * 100);
    sc.pct = (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
    sc.tick = (uint8_t)(tick < 0 ? 0 : tick > 100 ? 100 : tick);
    sc.hot = sc.pct >= sc.tick;
}

static void composeMain(Scene &sc, const session_view::State &s, uint32_t now)
{
    memset(&sc, 0, sizeof(sc));
    const Settings &set = settings::get();
    if (set.chan[0]) fmt(sc.chip, sizeof(sc.chip), "%s \xC2\xB7 %u", set.chan, set.freqMhz);
    else fmt(sc.chip, sizeof(sc.chip), "%u MHZ", set.freqMhz);

    if (!s.armed)
    {
        sc.kind = K_IDLE;
        bool ble = set.links & LINK_BLE, espnow = set.links & LINK_ESPNOW;
        fmt(sc.radio, sizeof(sc.radio), "%s", ble && espnow ? "BLE + ESP-NOW" : ble ? "BLE" : espnow ? "ESP-NOW" : "USB");
        if (set.msg.event && set.event[0])
        {
            fmt(sc.event, sizeof(sc.event), "%s", set.event);
            fitText(sc.event, sizeof(sc.event), art::PLEX12, 166);
        }
        fmt(sc.heatBig, sizeof(sc.heatBig), "%s %u", set.heatPrefix[0] ? set.heatPrefix : "Heat", settings::nextHeat());
        fitText(sc.heatBig, sizeof(sc.heatBig), art::CHAKRA46, 172);
        if (set.mode == race::RACE)
        {
            fmt(sc.mode, sizeof(sc.mode), "RACE");
            limitText(set, sc.sub, sizeof(sc.sub));
        }
        else
        {
            fmt(sc.mode, sizeof(sc.mode), "PRACTICE");
            fmt(sc.sub, sizeof(sc.sub), "no limit");
        }
        sc.haveLast = s.haveLast;
        if (s.haveLast)
        {
            fmt(sc.lastTitle, sizeof(sc.lastTitle), "Heat %u", s.lastHeat);
            fmt(sc.lastLaps, sizeof(sc.lastLaps), "%u laps", s.lastLaps);
            lapTime(s.lastBestMs, sc.a.value, sizeof(sc.a.value));
            lapTime(s.lastAvgMs, sc.b.value, sizeof(sc.b.value));
        }
        else
        {
            fmt(sc.lastTitle, sizeof(sc.lastTitle), "Last session");
            fmt(sc.a.value, sizeof(sc.a.value), "-");
            fmt(sc.b.value, sizeof(sc.b.value), "-");
        }
        return;
    }

    bool isRace = s.mode == race::RACE;
    const char *prefix = set.heatPrefix[0] ? set.heatPrefix : "Heat";
    fmt(sc.heat, sizeof(sc.heat), "%s %u", prefix, s.heat);
    fitText(sc.heat, sizeof(sc.heat), art::PLEX12, 100);
    signalBar(sc, set);
    sc.hint = 1;

    if (s.stage == race::FINISHED)
    {
        sc.kind = K_FINISHED;
        sc.dot = 1;
        fmt(sc.status, sizeof(sc.status), "FINISHED \xC2\xB7 %s", isRace ? "RACE" : "PRACTICE");
        clockTime(s.raceMs ? s.raceMs : s.sumMs, sc.clock, sizeof(sc.clock));
        fmt(sc.lapWord, sizeof(sc.lapWord), "LAPS");
        fmt(sc.lapNo, sizeof(sc.lapNo), "%u", s.laps);
        if (s.lapLimit) fmt(sc.lapTotal, sizeof(sc.lapTotal), "/%u", s.lapLimit);
        fmt(sc.a.label, sizeof(sc.a.label), "Best \xC2\xB7 lap %u", s.bestLap);
        lapTime(s.bestMs, sc.a.value, sizeof(sc.a.value));
        sc.a.tone = T_BEST;
        fmt(sc.b.label, sizeof(sc.b.label), "Average");
        lapTime(s.laps ? s.sumMs / s.laps : 0, sc.b.value, sizeof(sc.b.value));
        return;
    }

    fmt(sc.status, sizeof(sc.status), "ARMED \xC2\xB7 %s", isRace ? "RACE" : "PRACTICE");

    if (!s.started)
    {
        sc.kind = K_WAIT;
        if (s.stage == race::STAGING)
        {
            char limit[16], t[24];
            limitText(set, limit, sizeof(limit));
            fmt(sc.lapWord, sizeof(sc.lapWord), "STAGING");
            upper(set.msg.stage, t, sizeof(t));
            fmt(sc.big, sizeof(sc.big), "%s", t);
            fmt(sc.sub, sizeof(sc.sub), "%s \xC2\xB7 GO comes in %s", limit, set.randomHold ? "1 to 3 s" : "2 s");
        }
        else
        {
            char t[24];
            fmt(sc.lapWord, sizeof(sc.lapWord), "LAP 1");
            if (s.stage == race::RACING) upper(set.msg.go, t, sizeof(t));
            else fmt(t, sizeof(t), "READY");
            fmt(sc.big, sizeof(sc.big), "%s", t);
            fmt(sc.sub, sizeof(sc.sub), "%s", s.stage == race::RACING ? "fly through the gate" : "fly through the gate to start");
            sc.hint = 0;
        }
        if (textWidth(art::CHAKRA54, sc.big, 35) > 300) fitText(sc.big, sizeof(sc.big), art::CHAKRA54, 300, 35);
        return;
    }

    sc.kind = K_LIVE;
    if (s.laps)
    {
        fmt(sc.a.label, sizeof(sc.a.label), "Last lap");
        lapTime(s.lastMs, sc.a.value, sizeof(sc.a.value));
        if (s.hasDelta)
        {
            deltaTime(s.deltaMs, sc.a.delta, sizeof(sc.a.delta));
            sc.a.deltaTone = s.deltaMs < 0 ? 1 : 2;
        }
        fmt(sc.b.label, sizeof(sc.b.label), "Best \xC2\xB7 lap %u", s.bestLap);
        lapTime(s.bestMs, sc.b.value, sizeof(sc.b.value));
    }
    else
    {
        fmt(sc.a.label, sizeof(sc.a.label), "Last lap");
        fmt(sc.a.value, sizeof(sc.a.value), "-");
        fmt(sc.b.label, sizeof(sc.b.label), "Best");
        fmt(sc.b.value, sizeof(sc.b.value), "-");
    }
    sc.b.tone = T_BEST;

    if (s.laps && (int32_t)(s.holdUntilMs - now) > 0)
    {
        // The lap just flown, held for a moment, and how it compared with the best before it.
        sc.held = 1;
        lapTime(s.lastMs, sc.clock, sizeof(sc.clock));
        fmt(sc.lapDone, sizeof(sc.lapDone), "LAP %u DONE", s.lastLap);
        if (s.lastBest)
        {
            sc.heldTone = T_BEST;
            fmt(sc.tag, sizeof(sc.tag), "NEW BEST");
        }
        else if (s.hasDelta)
        {
            sc.heldTone = T_SLOW;
            deltaTime(s.deltaMs, sc.tag, sizeof(sc.tag));
        }
        return;
    }
    clockTime(now - s.lapAtMs, sc.clock, sizeof(sc.clock));
    fmt(sc.lapWord, sizeof(sc.lapWord), "LAP");
    fmt(sc.lapNo, sizeof(sc.lapNo), "%u", s.laps + 1);
    if (isRace && s.lapLimit) fmt(sc.lapTotal, sizeof(sc.lapTotal), "/%u", s.lapLimit);
}

// --- Boot splash -----------------------------------------------------------------------
// The mark with its arcs lighting in turn (a bark), the wordmark, and a bar following the
// real start-up steps. It's paced to last at least SPLASH_MS even when start-up is quicker,
// then the main screen takes over; start-up never waits for it.

static constexpr uint32_t SPLASH_MS = 1800;
static constexpr uint32_t BARK_MS = 1200, ARC_STEP_MS = 140, ARC_RISE_MS = 70, FALL_AT_MS = 900, ARC_FALL_MS = 180;
static const char *volatile bootLabel = "";
static volatile int bootSteps = 0;
static volatile bool booting = true;
static uint32_t splashAt = 0;

static unsigned ramp(uint32_t t, uint32_t from, uint32_t to)
{
    if (t <= from) return 0;
    if (t >= to) return 255;
    return (t - from) * 255 / (to - from);
}

static void composeSplash(Scene &sc, uint32_t now, float &progress)
{
    memset(&sc, 0, sizeof(sc));
    sc.kind = K_SPLASH;
    uint32_t t = (now - splashAt) % BARK_MS;
    for (int i = 0; i < 3; i++)
    {
        unsigned up = ramp(t, i * ARC_STEP_MS, i * ARC_STEP_MS + ARC_RISE_MS);
        unsigned down = ramp(t, FALL_AT_MS, FALL_AT_MS + ARC_FALL_MS);
        sc.arcs[i] = (uint8_t)(up * (255 - down) / 255);
    }
    float target = booting ? bootSteps * 20.0f : 100.0f;
    progress += (target - progress) * 0.35f;
    sc.progress = (uint8_t)(progress + 0.5f);
    fmt(sc.label, sizeof(sc.label), "%s", booting ? bootLabel : "READY");
}

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

static void task(void *)
{
    // On the heap: static data sits below the SDR's capture bank, and there's no room left there.
    Scene *shown = (Scene *)calloc(1, sizeof(Scene)), *next = (Scene *)calloc(1, sizeof(Scene));
    if (!shown || !next) vTaskDelete(nullptr);
    bool first = true;
    float progress = 0;
    while (true)
    {
        uint32_t now = millis();
        if (booting || now - splashAt < SPLASH_MS) composeSplash(*next, now, progress);
        else composeMain(*next, session_view::get(), now);
        if (first || memcmp(shown, next, sizeof(Scene)))
        {
            present(*next);
            memcpy(shown, next, sizeof(Scene));
            if (first) backlight(100); // the first picture is in: switch the light on
            first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void display::splash()
{
    if (!panelInit())
    {
        ESP_LOGE(TAG, "panel did not start");
        return;
    }
    esp_lcd_panel_disp_on_off(panel, true);
    splashAt = millis();
    xTaskCreate(task, "screen", 5120, nullptr, 1, nullptr);
}

void display::bootStep(const char *label)
{
    bootLabel = label;
    bootSteps = bootSteps + 1;
}

void display::bootDone() { booting = false; }

void display::begin() {} // the screen task started with the splash

void display::frame(const lapframe::Frame &f) { session_view::frame(f); }

#endif // LAPDOG_LCD147
