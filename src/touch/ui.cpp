// The touch timer's screens (Waveshare ESP32-C5-Touch-LCD-3.5, 480x320), after
// the "LapDog Touch Timer" design: idle (arm), live timing, a settings grid
// and one page per category, plus website sync (cloud.h): Wi-Fi, pairing and
// the Sync button. Everything the pilot changes here is a timer
// setting, saved to NVS straight away - the same settings the app and the USB
// console change. Settings are only reachable while idle; arming (from here,
// the BOOT button or USB) always brings up the live screen.
//
// display::frame() runs on the timing loop: it only updates a small model under
// a spinlock. LVGL's own task redraws from that model every UI_TICK_MS.

#if LAPDOG_TOUCH

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "art.h"
#include "board.h"
#include "channels.h"
#include "cloud.h"
#include "config.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "heatstore.h"
#include "power.h"
#include "race.h"
#include "calibration.h"
#include "presets.h"
#include "settings.h"
#include "splash.h"
#include "theme.h"

static const char *TAG = "ui";
static constexpr uint32_t UI_TICK_MS = 50;

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

// --- Model: what the session has said, from its frames ---------------------------------

struct Model
{
    bool armed;
    uint8_t mode, stage;
    uint16_t heat;
    uint8_t lapLimit;
    uint16_t timeLimitS;
    bool started, finished;
    uint32_t lapStartMs; // local clock at the last pass (the running lap starts there)
    uint8_t laps;
    uint32_t lastMs, bestMs, sumMs, totalMs;
    uint8_t bestLap;
    uint8_t recentLap[3];
    uint32_t recentMs[3];
    // the last finished session, for the idle screen
    bool haveLast;
    uint16_t lastHeat;
    uint8_t lastLaps;
    uint32_t lastBest, lastAvg;
};

static Model model = {};
static portMUX_TYPE modelLock = portMUX_INITIALIZER_UNLOCKED;

void display::frame(const lapframe::Frame &f)
{
    uint32_t now = millis();
    taskENTER_CRITICAL(&modelLock);
    Model &m = model;
    switch (f.type)
    {
    case lapframe::ARMED:
    {
        bool haveLast = m.haveLast;
        uint16_t lh = m.lastHeat;
        uint8_t ll = m.lastLaps;
        uint32_t lb = m.lastBest, la = m.lastAvg;
        m = {};
        m.haveLast = haveLast;
        m.lastHeat = lh;
        m.lastLaps = ll;
        m.lastBest = lb;
        m.lastAvg = la;
        m.armed = true;
        m.mode = f.mode;
        m.heat = f.heat;
        m.lapLimit = f.lapLimit;
        m.timeLimitS = f.timeLimitS;
        m.stage = f.mode == race::RACE ? race::STAGING : race::PRACTICE_RUN;
        break;
    }
    case lapframe::RACE:
        m.stage = f.stage;
        if (f.stage == race::FINISHED)
        {
            m.finished = true;
            m.totalMs = f.ms;
        }
        break;
    case lapframe::HEARTBEAT:
        if (m.armed) m.stage = f.stage;
        break;
    case lapframe::LAP:
        if (f.lap == 0) m.started = true;
        else
        {
            m.laps = f.lap;
            m.lastMs = f.ms;
            m.sumMs += f.ms;
            if (f.flags & lapframe::LAP_BEST)
            {
                m.bestMs = f.ms;
                m.bestLap = f.lap;
            }
            memmove(m.recentLap + 1, m.recentLap, 2);
            memmove(m.recentMs + 1, m.recentMs, 2 * sizeof(uint32_t));
            m.recentLap[0] = f.lap;
            m.recentMs[0] = f.ms;
            if (f.flags & lapframe::LAP_FINAL) m.finished = true;
        }
        m.lapStartMs = now;
        break;
    case lapframe::DISARMED:
        if (m.armed && m.laps)
        {
            m.haveLast = true;
            m.lastHeat = m.heat;
            m.lastLaps = m.laps;
            m.lastBest = m.bestMs;
            m.lastAvg = m.sumMs / m.laps;
        }
        m.armed = false;
        m.stage = race::IDLE;
        break;
    default: break;
    }
    taskEXIT_CRITICAL(&modelLock);
}

static Model snapshot()
{
    taskENTER_CRITICAL(&modelLock);
    Model m = model;
    taskEXIT_CRITICAL(&modelLock);
    return m;
}

// --- Formatting -------------------------------------------------------------------------

// "5.052" under a minute, "1:02.310" after.
static void fmtLap(uint32_t ms, char *out, size_t len)
{
    if (ms < 60000) snprintf(out, len, "%lu.%03lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    else snprintf(out, len, "%lu:%02lu.%03lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60),
                  (unsigned long)(ms % 1000));
}

// The running clock: "0:03.21".
static void fmtClock(uint32_t ms, char *out, size_t len)
{
    snprintf(out, len, "%lu:%02lu.%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60),
             (unsigned long)(ms % 1000 / 10));
}

static void fmtMinutes(int s, char *out, size_t len) { snprintf(out, len, "%d:%02d", s / 60, s % 60); }

// --- Widget helpers ---------------------------------------------------------------------

static void fill(lv_obj_t *o, uint32_t bg, uint32_t border, int radius)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    if (border)
    {
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(o, 1, 0);
    }
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, uint32_t border, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    fill(o, bg, border, radius);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, s);
    return l;
}

// A tappable surface; darker while pressed.
static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, uint32_t border, int radius,
                        lv_event_cb_t cb, void *user = nullptr)
{
    lv_obj_t *b = panel(parent, x, y, w, h, bg, border, radius);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(col::SELECTED), LV_STATE_PRESSED);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

// Selected / not selected look for chips, tabs and segments.
static void setSelected(lv_obj_t *b, bool on, bool solid = false)
{
    if (solid) fill(b, on ? col::ACCENT : col::SURFACE, on ? col::ACCENT : col::LINE, lv_obj_get_style_radius(b, LV_PART_MAIN));
    else fill(b, on ? col::SELECTED : col::SURFACE, on ? col::ACCENT : col::LINE, lv_obj_get_style_radius(b, LV_PART_MAIN));
    lv_obj_set_style_border_width(b, on && !solid ? 2 : 1, 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    for (; l; l = lv_obj_get_sibling(l, 1))
        if (lv_obj_check_type(l, &lv_label_class))
            lv_obj_set_style_text_color(l, lv_color_hex(on ? (solid ? col::ON_ACCENT : col::SELECTED_TEXT) : col::TEXT), 0);
}

static lv_obj_t *screen()
{
    lv_obj_t *s = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s);
    fill(s, col::BG, 0, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

// --- Screens ------------------------------------------------------------------------------

enum Screen
{
    S_IDLE,
    S_LIVE,
    S_SETTINGS,
    S_CHANNEL,
    S_SENSITIVITY,
    S_FALSE_READS,
    S_RADIO,
    S_MESSAGES,
    S_RACE,
    S_HISTORY,
    S_HEAT,
    S_KEYBOARD,
    S_SYNC,
    S_WIFI,
    S_WIFI_KEYS,
    S_PAIR,
    S_COUNT
};

static lv_obj_t *screens[S_COUNT];
static Screen current = S_IDLE;
static void show(Screen s);
static void refresh(Screen s);

static void goBack(lv_event_t *e) { show((Screen)(intptr_t)lv_event_get_user_data(e)); }
static void goTo(lv_event_t *e) { show((Screen)(intptr_t)lv_event_get_user_data(e)); }

// A page's top bar: back, icon and title.
static void header(lv_obj_t *s, const char *icon, const char *title, Screen back)
{
    lv_obj_t *b = button(s, 4, 4, 48, 48, col::BG, 0, 24, goBack, (void *)(intptr_t)back);
    lv_obj_t *l = text(b, &lv_font_montserrat_20, col::TEXT, LV_SYMBOL_LEFT);
    lv_obj_center(l);
    if (icon)
    {
        lv_obj_t *t = panel(s, 54, 12, 32, 32, col::SELECTED, 0, 10);
        lv_obj_center(text(t, &lv_font_montserrat_16, col::ACCENT, icon));
    }
    lv_obj_t *h = text(s, &lv_font_montserrat_24, col::TEXT, title);
    lv_obj_set_pos(h, icon ? 96 : 56, 14);
}

// The UI's task has its stack in PSRAM, so the timing loop does the flash write.
static void save() { app::requestSave(); }

// --- Steppers and switches -------------------------------------------------------------

struct Stepper
{
    lv_obj_t *value;
    int (*get)();
    void (*set)(int);
    int step, min, max;
    void (*fmt)(int, char *, size_t);
};

static void stepperRefresh(Stepper *st)
{
    char b[24];
    st->fmt(st->get(), b, sizeof(b));
    lv_label_set_text(st->value, b);
}

static void stepBy(Stepper *st, int dir)
{
    int v = st->get();
    // Land on multiples of the step, so an odd value set elsewhere snaps onto the grid.
    v = dir > 0 ? (v / st->step + 1) * st->step : ((v - 1) / st->step) * st->step;
    if (v < st->min) v = st->min;
    if (v > st->max) v = st->max;
    if (v == st->get()) return;
    st->set(v);
    save();
    stepperRefresh(st);
}

static void stepDown(lv_event_t *e) { stepBy((Stepper *)lv_event_get_user_data(e), -1); }
static void stepUp(lv_event_t *e) { stepBy((Stepper *)lv_event_get_user_data(e), 1); }

// [-] value [+] at x,y (172 wide). Hold a button to repeat.
static void stepper(lv_obj_t *parent, int x, int y, Stepper *st)
{
    lv_obj_t *minus = button(parent, x, y, 48, 44, col::BG, col::LINE, 10, nullptr);
    lv_obj_add_event_cb(minus, stepDown, LV_EVENT_SHORT_CLICKED, st);
    lv_obj_add_event_cb(minus, stepDown, LV_EVENT_LONG_PRESSED_REPEAT, st);
    lv_obj_center(text(minus, &lv_font_montserrat_24, col::TEXT, "-"));
    st->value = text(parent, &lv_font_montserrat_20, col::TEXT, "");
    lv_obj_set_width(st->value, 76);
    lv_obj_set_style_text_align(st->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(st->value, x + 48, y + 11);
    lv_obj_t *plus = button(parent, x + 124, y, 48, 44, col::BG, col::LINE, 10, nullptr);
    lv_obj_add_event_cb(plus, stepUp, LV_EVENT_SHORT_CLICKED, st);
    lv_obj_add_event_cb(plus, stepUp, LV_EVENT_LONG_PRESSED_REPEAT, st);
    lv_obj_center(text(plus, &lv_font_montserrat_24, col::TEXT, "+"));
}

// A settings row inside a card: title and optional subtitle on the left.
static void rowTitle(lv_obj_t *card, int y, const char *title, const char *sub)
{
    lv_obj_t *t = text(card, &lv_font_montserrat_16, col::TEXT, title);
    lv_obj_set_pos(t, 14, sub ? y + 6 : y + 14);
    if (sub)
    {
        lv_obj_t *s = text(card, &lv_font_montserrat_12, col::MUTED, sub);
        lv_obj_set_pos(s, 14, y + 26);
    }
}

static void rowDivider(lv_obj_t *card, int y) { panel(card, 12, y, lv_obj_get_width(card) - 24, 1, col::DIVIDER, 0, 0); }

struct Toggle
{
    lv_obj_t *sw;
    bool (*get)();
    void (*set)(bool);
};

static void toggled(lv_event_t *e)
{
    Toggle *t = (Toggle *)lv_event_get_user_data(e);
    t->set(lv_obj_has_state(t->sw, LV_STATE_CHECKED));
    save();
}

static void toggle(lv_obj_t *parent, int x, int y, Toggle *t)
{
    t->sw = lv_switch_create(parent);
    lv_obj_set_size(t->sw, 56, 30);
    lv_obj_set_pos(t->sw, x, y);
    lv_obj_set_style_bg_color(t->sw, lv_color_hex(col::OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(t->sw, lv_color_hex(col::ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(t->sw, toggled, LV_EVENT_VALUE_CHANGED, t);
}

static void toggleRefresh(Toggle *t)
{
    if (t->get()) lv_obj_add_state(t->sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(t->sw, LV_STATE_CHECKED);
}

// --- Battery ------------------------------------------------------------------------------
// The live screen shows a level symbol in a pill (the idle screen has its own icon, below;
// the percentage is in the Settings header), a bolt beside it while charging, the USB symbol when there's no
// battery. Red below 15 %. Read from the
// AXP2101 (power.h) every few seconds by tick().

static constexpr int LOW_BATTERY_PERCENT = 15, VERY_LOW_BATTERY_PERCENT = 5;
static const char *levelSymbol(int pct)
{
    if (pct >= 88) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 63) return LV_SYMBOL_BATTERY_3;
    if (pct >= 38) return LV_SYMBOL_BATTERY_2;
    if (pct >= 13) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void showBattery(lv_obj_t *label, uint32_t normal)
{
    power::State b = power::state();
    char t[24] = "";
    uint32_t color = normal;
    if (b.valid && b.battery && b.percent >= 0)
    {
        // The level as a symbol alone (the percentage is on the Settings tile), with a bolt beside it while charging.
        snprintf(t, sizeof(t), "%s%s", b.charging ? LV_SYMBOL_CHARGE " " : "", levelSymbol(b.percent));
        if (b.percent < LOW_BATTERY_PERCENT && !b.charging) color = col::ALERT;
    }
    else if (b.valid && b.vbus) snprintf(t, sizeof(t), LV_SYMBOL_USB);
    lv_label_set_text(label, t);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_t *pill = lv_obj_get_parent(label);
    if (t[0]) lv_obj_remove_flag(pill, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
}

// The idle screen's battery: a vertical icon drawn from plain shapes (body, terminal, a fill
// that rises with the level), no pill. A bolt over it while charging; the USB symbol in its
// place when there's no battery. Red below 15 %.
struct BatteryIcon
{
    lv_obj_t *root, *fill, *bolt, *usb, *body;
};

static constexpr int BAT_W = 12, BAT_H = 24, BAT_INNER_W = 8, BAT_INNER_H = 18;

static BatteryIcon batteryIcon(lv_obj_t *parent, int x, int y)
{
    BatteryIcon b = {};
    b.root = lv_obj_create(parent);
    lv_obj_remove_style_all(b.root);
    lv_obj_set_pos(b.root, x, y);
    lv_obj_set_size(b.root, BAT_W, BAT_H);
    lv_obj_remove_flag(b.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b.root, LV_OBJ_FLAG_CLICKABLE);
    panel(b.root, 4, 0, 4, 2, col::SELECTED_TEXT, 0, 1); // the terminal
    b.body = panel(b.root, 0, 2, BAT_W, BAT_H - 2, col::BG, col::SELECTED_TEXT, 3);
    lv_obj_set_style_border_width(b.body, 2, 0);
    b.fill = panel(b.body, 0, 0, BAT_INNER_W, BAT_INNER_H, col::SELECTED_TEXT, 0, 2);
    b.bolt = text(b.root, &lv_font_montserrat_12, col::BG, LV_SYMBOL_CHARGE);
    lv_obj_align(b.bolt, LV_ALIGN_CENTER, 0, 1);
    b.usb = text(b.root, &lv_font_montserrat_16, col::SELECTED_TEXT, LV_SYMBOL_USB);
    lv_obj_center(b.usb);
    return b;
}

static void showBatteryIcon(const BatteryIcon &b)
{
    power::State p = power::state();
    bool has = p.valid && p.battery && p.percent >= 0;
    bool usb = !has && p.valid && p.vbus;
    lv_obj_set_flag(b.body, LV_OBJ_FLAG_HIDDEN, !has);
    lv_obj_set_flag(b.bolt, LV_OBJ_FLAG_HIDDEN, !(has && p.charging));
    lv_obj_set_flag(b.usb, LV_OBJ_FLAG_HIDDEN, !usb);
    // The terminal is the root's first child.
    lv_obj_set_flag(lv_obj_get_child(b.root, 0), LV_OBJ_FLAG_HIDDEN, !has);
    if (!has) return;
    uint32_t color = p.percent < LOW_BATTERY_PERCENT && !p.charging ? col::ALERT : col::SELECTED_TEXT;
    int h = BAT_INNER_H * p.percent / 100;
    if (h < 2) h = 2;
    if (h > BAT_INNER_H) h = BAT_INNER_H;
    lv_obj_set_size(b.fill, BAT_INNER_W, h);
    lv_obj_set_y(b.fill, BAT_INNER_H - h);
    lv_obj_set_style_bg_color(b.fill, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(b.body, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(lv_obj_get_child(b.root, 0), lv_color_hex(color), 0);
    // Dark bolt on a fill that reaches it, light on the dark body.
    lv_obj_set_style_text_color(b.bolt, lv_color_hex(p.percent >= 45 ? col::BG : col::TEXT), 0);
}

// --- Idle -------------------------------------------------------------------------------

static struct
{
    lv_obj_t *chan, *radio, *event, *heat, *practice, *race, *raceLabel, *armHint;
    BatteryIcon battery;
    lv_obj_t *lastTitle, *lastLaps, *lastBest, *lastAvg, *histCount;
} idle;

static void openHistory(lv_event_t *);
static void openNewest(lv_event_t *);

static void pickMode(lv_event_t *e)
{
    Settings &s = settings::get();
    s.mode = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    save();
    refresh(S_IDLE);
}

static void armNow(lv_event_t *)
{
    app::ArmRequest r = {};
    r.mode = (race::Mode)settings::get().mode;
    app::requestArm(r);
}

// Long-press the heat name to start the heat numbering again at 1. The press only asks:
// a tap on the name within 4 s says yes (the same way Unpair confirms), so a stray press
// can't throw the number away. Idle only: this screen isn't shown while armed.
static uint32_t heatResetAskedAt; // millis() of the long press, 0 = not asking
static bool askingHeatReset() { return heatResetAskedAt && millis() - heatResetAskedAt < 4000; }

static void heatNameLongPress(lv_event_t *)
{
    if (app::armed()) return;
    heatResetAskedAt = millis();
    refresh(S_IDLE);
}

static void heatNameTap(lv_event_t *)
{
    // The release after a long press isn't an answer.
    if (!askingHeatReset() || millis() - heatResetAskedAt < 600 || app::armed()) return;
    heatResetAskedAt = 0;
    settings::setNextHeatUnsaved(1);
    save();
    refresh(S_IDLE);
}

static void chip(lv_obj_t *s, lv_obj_t **label, int x, int w)
{
    lv_obj_t *c = panel(s, x, 12, w, 30, col::SURFACE, col::LINE, 15);
    *label = text(c, &lv_font_montserrat_14, col::SELECTED_TEXT, "");
    lv_obj_center(*label);
}

static void buildIdle()
{
    lv_obj_t *s = screens[S_IDLE] = screen();
    lv_obj_t *logo = lv_image_create(s);
    lv_image_set_src(logo, &art::lockup);
    lv_obj_set_pos(logo, 16, 12); // level with the chips
    chip(s, &idle.chan, 216, 110);
    chip(s, &idle.radio, 332, 60);
    idle.battery = batteryIcon(s, 402, 15); // close beside the radio pill and the cog
    lv_obj_t *gear = button(s, 420, 4, 48, 48, col::BG, 0, 24, goTo, (void *)(intptr_t)S_SETTINGS);
    lv_obj_center(text(gear, &lv_font_montserrat_24, col::TEXT, LV_SYMBOL_SETTINGS));

    idle.event = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_set_pos(idle.event, 16, 58);
    idle.heat = text(s, &lv_font_montserrat_48, col::TEXT, "");
    lv_obj_set_pos(idle.heat, 14, 74);
    lv_obj_add_flag(idle.heat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(idle.heat, 12);
    lv_obj_add_event_cb(idle.heat, heatNameLongPress, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(idle.heat, heatNameTap, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *seg = panel(s, 16, 136, 284, 50, col::SURFACE, col::LINE, 12);
    idle.practice = button(seg, 4, 4, 136, 40, col::SURFACE, 0, 9, pickMode, (void *)(intptr_t)race::PRACTICE);
    lv_obj_center(text(idle.practice, &lv_font_montserrat_16, col::TEXT, "Practice"));
    idle.race = button(seg, 142, 4, 136, 40, col::SURFACE, 0, 9, pickMode, (void *)(intptr_t)race::RACE);
    idle.raceLabel = text(idle.race, &lv_font_montserrat_16, col::TEXT, "Race");
    lv_obj_center(idle.raceLabel);

    // The finished heats the board has kept (history), in the free band above the last session.
    lv_obj_t *hist = button(s, 16, 194, 284, 40, col::SURFACE, col::LINE, 12, openHistory);
    lv_obj_t *hi = text(hist, &lv_font_montserrat_16, col::ACCENT, LV_SYMBOL_LIST);
    lv_obj_set_pos(hi, 14, 10);
    lv_obj_t *ht = text(hist, &lv_font_montserrat_16, col::TEXT, "History");
    lv_obj_set_pos(ht, 44, 10);
    idle.histCount = text(hist, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_align(idle.histCount, LV_ALIGN_RIGHT_MID, -14, 0);

    lv_obj_t *card = panel(s, 16, 240, 284, 64, col::SURFACE, col::LINE, 12);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE); // the newest heat, with its laps
    lv_obj_add_event_cb(card, openNewest, LV_EVENT_CLICKED, nullptr);
    const char *titles[] = {"", "Best", "Average"};
    lv_obj_t **values[] = {&idle.lastLaps, &idle.lastBest, &idle.lastAvg};
    for (int i = 0; i < 3; i++)
    {
        lv_obj_t *t = text(card, &lv_font_montserrat_12, col::MUTED, titles[i]);
        lv_obj_set_pos(t, 12 + i * 92, 10);
        if (i == 0) idle.lastTitle = t;
        *values[i] = text(card, &lv_font_montserrat_20, i == 1 ? col::BEST : col::TEXT, "");
        lv_obj_set_pos(*values[i], 12 + i * 92, 30);
    }

    lv_obj_t *arm = button(s, 314, 62, 150, 242, col::ACCENT, 0, 20, armNow);
    lv_obj_set_style_bg_color(arm, lv_color_hex(0x2F7FC2), LV_STATE_PRESSED);
    lv_obj_t *icon = text(arm, &lv_font_montserrat_40, col::ON_ACCENT, LV_SYMBOL_PLAY);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -44);
    lv_obj_t *label = text(arm, &lv_font_montserrat_40, col::ON_ACCENT, "ARM");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 10);
    idle.armHint = text(arm, &lv_font_montserrat_12, col::ON_ACCENT, "");
    lv_obj_set_width(idle.armHint, 130);
    lv_obj_set_style_text_align(idle.armHint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(idle.armHint, LV_ALIGN_CENTER, 0, 52);
}

static void refreshChannelChip(lv_obj_t *l)
{
    const Settings &s = settings::get();
    lv_label_set_text_fmt(l, "%s %u", s.chan[0] ? s.chan : "", s.freqMhz);
}

static void refreshIdle()
{
    const Settings &s = settings::get();
    refreshChannelChip(idle.chan);
    showBatteryIcon(idle.battery);
    char r[24] = "";
    if (s.links & LINK_BLE) strcat(r, LV_SYMBOL_BLUETOOTH);
    if (s.links & LINK_ESPNOW) strcat(r, s.links & LINK_BLE ? " " LV_SYMBOL_WIFI : LV_SYMBOL_WIFI);
    lv_label_set_text(idle.radio, r[0] ? r : LV_SYMBOL_USB);

    char ev[race::EVENT_MAX + 1];
    race::upper(s.event, ev, sizeof(ev));
    lv_label_set_text(idle.event, s.msg.event ? ev : "");
    if (askingHeatReset())
    {
        lv_label_set_text(idle.heat, "Heat to 1?");
        lv_obj_set_style_text_color(idle.heat, lv_color_hex(col::ALERT), 0);
        lv_label_set_text(idle.event, "TAP THE NAME AGAIN TO RESET THE HEAT NUMBER");
        lv_obj_set_style_text_color(idle.event, lv_color_hex(col::ALERT), 0);
    }
    else
    {
        heatResetAskedAt = 0;
        lv_obj_set_style_text_color(idle.heat, lv_color_hex(col::TEXT), 0);
        lv_obj_set_style_text_color(idle.event, lv_color_hex(col::MUTED), 0);
        lv_label_set_text_fmt(idle.heat, "%s %u", s.heatPrefix[0] ? s.heatPrefix : "Heat", settings::nextHeat());
    }

    bool race = s.mode == race::RACE;
    setSelected(idle.practice, !race, true);
    setSelected(idle.race, race, true);
    if (s.raceLimit == race::LIMIT_LAPS) lv_label_set_text_fmt(idle.raceLabel, "Race - %u laps", s.raceLaps);
    else
    {
        char t[12];
        fmtMinutes(s.raceTimeS, t, sizeof(t));
        lv_label_set_text_fmt(idle.raceLabel, "Race - %s", t);
    }
    power::State pw = power::state();
    bool veryLow = pw.valid && pw.battery && pw.percent >= 0 && pw.percent < VERY_LOW_BATTERY_PERCENT && !pw.charging;
    lv_label_set_text(idle.armHint, veryLow ? "battery very low: a heat may not last" : cloud::busy() ? "website sync has the radio" : race ? "stage, then GO" : "first pass starts lap 1");

    heatstore::Usage hu = heatstore::usage();
    if (!heatstore::ready()) lv_label_set_text(idle.histCount, "no storage");
    else lv_label_set_text_fmt(idle.histCount, "%d heat%s", hu.waiting + hu.sent, hu.waiting + hu.sent == 1 ? "" : "s");

    Model m = snapshot();
    char b[16];
    if (m.haveLast)
    {
        lv_label_set_text_fmt(idle.lastTitle, "Heat %u", m.lastHeat);
        lv_label_set_text_fmt(idle.lastLaps, "%u laps", m.lastLaps);
        fmtLap(m.lastBest, b, sizeof(b));
        lv_label_set_text(idle.lastBest, b);
        fmtLap(m.lastAvg, b, sizeof(b));
        lv_label_set_text(idle.lastAvg, b);
    }
    else
    {
        lv_label_set_text(idle.lastTitle, "Last session");
        lv_label_set_text(idle.lastLaps, "-");
        lv_label_set_text(idle.lastBest, "-");
        lv_label_set_text(idle.lastAvg, "-");
    }
}

// --- Live -------------------------------------------------------------------------------

static struct
{
    lv_obj_t *status, *battery, *chan, *lapNo, *lapWord, *clock, *last, *best, *bestTitle;
    lv_obj_t *recent[3], *recentLap[3], *recentBox[3], *signal, *signalDb;
} live;

static void disarmHeld(lv_event_t *) { app::requestDisarm(); }

static void buildLive()
{
    lv_obj_t *s = screens[S_LIVE] = screen();
    lv_obj_t *dot = panel(s, 18, 22, 10, 10, col::OK, 0, 5);
    lv_obj_set_style_outline_color(dot, lv_color_hex(col::OK_TINT), 0);
    lv_obj_set_style_outline_width(dot, 4, 0);
    live.status = text(s, &lv_font_montserrat_14, col::TEXT, "");
    lv_obj_set_pos(live.status, 38, 18);
    chip(s, &live.battery, 300, 48);
    chip(s, &live.chan, 354, 110);

    live.lapWord = text(s, &lv_font_montserrat_24, col::MUTED, "LAP");
    lv_obj_set_pos(live.lapWord, 16, 60);
    live.lapNo = text(s, &lv_font_montserrat_40, col::TEXT, "");
    lv_obj_set_pos(live.lapNo, 74, 50);
    live.clock = text(s, &lv_font_montserrat_48, col::TEXT, "");
    lv_obj_set_pos(live.clock, 14, 100);

    lv_obj_t *lastCard = panel(s, 16, 166, 140, 58, col::SURFACE, col::LINE, 12);
    lv_obj_set_pos(text(lastCard, &lv_font_montserrat_12, col::MUTED, "Last lap"), 10, 6);
    live.last = text(lastCard, &lv_font_montserrat_24, col::TEXT, "-");
    lv_obj_set_pos(live.last, 10, 24);
    lv_obj_t *bestCard = panel(s, 164, 166, 140, 58, col::BEST_TINT, col::BEST_EDGE, 12);
    live.bestTitle = text(bestCard, &lv_font_montserrat_12, col::BEST, "Best");
    lv_obj_set_pos(live.bestTitle, 10, 6);
    live.best = text(bestCard, &lv_font_montserrat_24, col::BEST, "-");
    lv_obj_set_pos(live.best, 10, 24);

    lv_obj_set_pos(text(s, &lv_font_montserrat_12, col::MUTED, "LAPS"), 318, 58);
    for (int i = 0; i < 3; i++)
    {
        live.recentBox[i] = panel(s, 316, 76 + i * 32, 148, 28, col::SURFACE, 0, 8);
        live.recentLap[i] = text(live.recentBox[i], &lv_font_montserrat_14, col::MUTED, "");
        lv_obj_align(live.recentLap[i], LV_ALIGN_LEFT_MID, 8, 0);
        live.recent[i] = text(live.recentBox[i], &lv_font_montserrat_16, col::TEXT, "");
        lv_obj_align(live.recent[i], LV_ALIGN_RIGHT_MID, -8, 0);
    }
    lv_obj_set_pos(text(s, &lv_font_montserrat_12, col::MUTED, "Signal"), 318, 184);
    live.signalDb = text(s, &lv_font_montserrat_12, col::TEXT, "");
    lv_obj_align(live.signalDb, LV_ALIGN_TOP_RIGHT, -16, 184);
    live.signal = lv_bar_create(s);
    lv_obj_set_pos(live.signal, 316, 204);
    lv_obj_set_size(live.signal, 148, 12);
    lv_bar_set_range(live.signal, 0, 450); // dB x10 above the receiver floor
    lv_obj_set_style_bg_color(live.signal, lv_color_hex(col::SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(live.signal, lv_color_hex(col::SOFT), LV_PART_INDICATOR);

    lv_obj_t *stop = button(s, 16, 250, 448, 54, col::BG, col::ACCENT, 14, nullptr);
    lv_obj_set_style_border_width(stop, 2, 0);
    lv_obj_set_style_bg_color(stop, lv_color_hex(col::ACCENT_RING), LV_STATE_PRESSED);
    lv_obj_add_event_cb(stop, disarmHeld, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_center(text(stop, &lv_font_montserrat_20, col::SELECTED_TEXT, "HOLD TO DISARM"));
}

static void refreshLive()
{
    Model m = snapshot();
    const Settings &s = settings::get();
    bool race = m.mode == race::RACE;
    lv_label_set_text_fmt(live.status, "ARMED - %s     Heat %u", race ? "RACE" : "PRACTICE", m.heat);
    showBattery(live.battery, col::SELECTED_TEXT);
    refreshChannelChip(live.chan);

    char b[24];
    if (m.finished)
    {
        lv_label_set_text(live.lapWord, "FINISHED");
        lv_label_set_text(live.lapNo, "");
        fmtClock(m.totalMs, b, sizeof(b));
        lv_label_set_text(live.clock, b);
    }
    else if (m.stage == race::STAGING)
    {
        lv_label_set_text(live.lapWord, "STAGING");
        lv_label_set_text(live.lapNo, "");
        lv_label_set_text(live.clock, s.msg.stage);
    }
    else if (!m.started)
    {
        lv_label_set_text(live.lapWord, race ? "GO!" : "READY");
        lv_label_set_text(live.lapNo, "");
        lv_label_set_text(live.clock, race ? "0:00.00" : "fly through");
    }
    else
    {
        lv_label_set_text(live.lapWord, "LAP");
        if (m.lapLimit) lv_label_set_text_fmt(live.lapNo, "%u/%u", m.laps + 1, m.lapLimit);
        else lv_label_set_text_fmt(live.lapNo, "%u", m.laps + 1);
        fmtClock(millis() - m.lapStartMs, b, sizeof(b));
        lv_label_set_text(live.clock, b);
    }
    if (m.laps)
    {
        fmtLap(m.lastMs, b, sizeof(b));
        lv_label_set_text(live.last, b);
        fmtLap(m.bestMs, b, sizeof(b));
        lv_label_set_text(live.best, b);
        lv_label_set_text_fmt(live.bestTitle, "Best - lap %u", m.bestLap);
    }
    else
    {
        lv_label_set_text(live.last, "-");
        lv_label_set_text(live.best, "-");
        lv_label_set_text(live.bestTitle, "Best");
    }
    for (int i = 0; i < 3; i++)
    {
        bool have = i < m.laps;
        lv_obj_set_flag(live.recentBox[i], LV_OBJ_FLAG_HIDDEN, !have);
        if (!have) continue;
        bool best = m.recentLap[i] == m.bestLap;
        fill(live.recentBox[i], best ? col::BEST_TINT : col::SURFACE, 0, 8);
        lv_label_set_text_fmt(live.recentLap[i], "L%u", m.recentLap[i]);
        fmtLap(m.recentMs[i], b, sizeof(b));
        lv_label_set_text(live.recent[i], b);
        lv_obj_set_style_text_color(live.recent[i], lv_color_hex(best ? col::BEST : col::TEXT), 0);
    }
    float db = app::signalDb();
    lv_bar_set_value(live.signal, (int32_t)(db < 0 ? 0 : db * 10), LV_ANIM_OFF);
    snprintf(b, sizeof(b), "%+.0f dB", db);
    lv_label_set_text(live.signalDb, b);
}

// --- Settings grid ------------------------------------------------------------------------

static constexpr int TILES = 7;

static struct
{
    lv_obj_t *value[TILES], *info;
} grid;

// A scrolling list of tiles, three to a row: it fits three rows as it is, and takes more
// tiles by scrolling (a finger drag, with momentum) rather than by running out of screen.
static void buildSettings()
{
    lv_obj_t *s = screens[S_SETTINGS] = screen();
    header(s, nullptr, "Settings", S_IDLE);
    grid.info = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_align(grid.info, LV_ALIGN_TOP_RIGHT, -16, 22);
    struct Tile
    {
        const char *icon, *title;
        Screen to;
    };
    static const Tile tiles[TILES] = {
        {LV_SYMBOL_WIFI, "Channel", S_CHANNEL},           {LV_SYMBOL_EYE_OPEN, "Sensitivity", S_SENSITIVITY},
        {LV_SYMBOL_WARNING, "False reads", S_FALSE_READS}, {LV_SYMBOL_BLUETOOTH, "Radio output", S_RADIO},
        {LV_SYMBOL_EDIT, "Race messages", S_MESSAGES},    {LV_SYMBOL_LOOP, "Race defaults", S_RACE},
        {LV_SYMBOL_UPLOAD, "Website sync", S_SYNC},
    };
    lv_obj_t *list = lv_obj_create(s);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 56);
    lv_obj_set_size(list, board::WIDTH, board::HEIGHT - 56);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(list, 10, 0);
    // Three rows of three: icon and title on top, what it's set to below.
    for (int i = 0; i < TILES; i++)
    {
        int x = 16 + (i % 3) * 152, y = 4 + (i / 3) * 86;
        lv_obj_t *t = button(list, x, y, 144, 78, col::SURFACE, col::LINE, 14, goTo, (void *)(intptr_t)tiles[i].to);
        lv_obj_t *ic = panel(t, 10, 10, 30, 30, col::SELECTED, 0, 9);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(text(ic, &lv_font_montserrat_16, col::ACCENT, tiles[i].icon));
        lv_obj_t *title = text(t, &lv_font_montserrat_14, col::TEXT, tiles[i].title);
        lv_obj_set_pos(title, 48, 16);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, 90);
        grid.value[i] = text(t, &lv_font_montserrat_12, col::SELECTED_TEXT, "");
        lv_obj_set_pos(grid.value[i], 10, 52);
        lv_label_set_long_mode(grid.value[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(grid.value[i], 124);
    }
}

// Presets are steps around the reference: the calibrated thresholds, else the defaults.
static presets::Kind presetOf(const DetectorParams &d)
{
    return presets::of(d.enterDb, d.exitDb, calibration::refEnterDb(), calibration::refExitDb());
}
static const char *sensitivityName(const DetectorParams &d) { return presets::name(presetOf(d)); }

static void refreshSettings()
{
    const Settings &s = settings::get();
    lv_label_set_text_fmt(grid.value[0], "%s - %u", s.chan[0] ? s.chan : "", s.freqMhz);
    lv_label_set_text_fmt(grid.value[1], "%s - +%.1f/+%.1f", sensitivityName(s.detector), s.detector.enterDb,
                          s.detector.exitDb);
    lv_label_set_text_fmt(grid.value[2], "%.1f s min lap", s.detector.minLapMs / 1000.0f);
    const char *radio = (s.links & LINK_BLE) && (s.links & LINK_ESPNOW) ? "BLE + ESP-NOW"
                        : (s.links & LINK_BLE)                          ? "Bluetooth"
                        : (s.links & LINK_ESPNOW)                       ? "ESP-NOW"
                                                                        : "USB only";
    lv_label_set_text(grid.value[3], radio);
    lv_label_set_text(grid.value[4], s.msg.event && s.event[0] ? s.event : "No event name");
    char t[12];
    fmtMinutes(s.raceTimeS, t, sizeof(t));
    if (s.raceLimit == race::LIMIT_LAPS) lv_label_set_text_fmt(grid.value[5], "%u laps - %s", s.raceLaps, t);
    else lv_label_set_text_fmt(grid.value[5], "%s race - %u laps", t, s.raceLaps);
    int waiting = cloud::pending();
    if (!cloud::paired()) lv_label_set_text(grid.value[6], "Not paired");
    else if (!cloud::storeReady()) lv_label_set_text(grid.value[6], "No storage");
    else if (waiting > 0) lv_label_set_text_fmt(grid.value[6], "%d heat%s waiting", waiting, waiting == 1 ? "" : "s");
    else lv_label_set_text(grid.value[6], "Up to date");
    // The battery has no tile: it's only a reading, here beside the firmware version.
    power::State pw = power::state();
    char bat[40];
    if (!pw.valid) snprintf(bat, sizeof(bat), "No battery reading");
    else if (!pw.battery) snprintf(bat, sizeof(bat), pw.vbus ? "USB power" : "No battery");
    else if (pw.charging) snprintf(bat, sizeof(bat), "Battery %d%% - charging", pw.percent);
    else snprintf(bat, sizeof(bat), "Battery %d%% - %.2f V", pw.percent, pw.mv / 1000.0f);
    lv_label_set_text_fmt(grid.info, "%s  -  Firmware " FIRMWARE_VERSION, bat);
}

// --- Channel ------------------------------------------------------------------------------

static struct
{
    lv_obj_t *current, *band[channels::BAND_COUNT], *chan[8], *chanId[8], *chanMhz[8], *bandName;
    int shown; // band index on screen
} chanPage;

static int bandOf(const Settings &s)
{
    for (int i = 0; i < channels::BAND_COUNT; i++)
        if (s.chan[0] == channels::BANDS[i].id) return i;
    return 0;
}

static void pickBand(lv_event_t *e)
{
    chanPage.shown = (int)(intptr_t)lv_event_get_user_data(e);
    refresh(S_CHANNEL);
}

static void pickChannel(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    char label[3] = {channels::BANDS[chanPage.shown].id, (char)('1' + ch), 0};
    if (timer_settings::set(settings::get(), "chan", label)) save();
    refresh(S_CHANNEL);
}

static void buildChannel()
{
    lv_obj_t *s = screens[S_CHANNEL] = screen();
    header(s, LV_SYMBOL_WIFI, "Video channel", S_SETTINGS);
    chanPage.current = text(s, &lv_font_montserrat_16, col::ACCENT, "");
    lv_obj_align(chanPage.current, LV_ALIGN_TOP_RIGHT, -16, 20);
    for (int i = 0; i < channels::BAND_COUNT; i++)
    {
        lv_obj_t *b = chanPage.band[i] = button(s, 16 + i * 75, 60, 71, 44, col::SURFACE, col::LINE, 10, pickBand,
                                                (void *)(intptr_t)i);
        char id[2] = {channels::BANDS[i].id, 0};
        lv_obj_center(text(b, &lv_font_montserrat_16, col::TEXT, id));
    }
    for (int i = 0; i < 8; i++)
    {
        lv_obj_t *b = chanPage.chan[i] = button(s, 16 + (i % 4) * 113, 114 + (i / 4) * 84, 108, 78, col::SURFACE,
                                                col::LINE, 12, pickChannel, (void *)(intptr_t)i);
        chanPage.chanId[i] = text(b, &lv_font_montserrat_20, col::TEXT, "");
        lv_obj_align(chanPage.chanId[i], LV_ALIGN_CENTER, 0, -10);
        chanPage.chanMhz[i] = text(b, &lv_font_montserrat_14, col::TEXT, "");
        lv_obj_align(chanPage.chanMhz[i], LV_ALIGN_CENTER, 0, 14);
    }
    chanPage.bandName = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_set_pos(chanPage.bandName, 16, 290);
    lv_obj_t *hint = text(s, &lv_font_montserrat_12, col::MUTED, "Mid-band channels detect best");
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -16, 290);
}

static void refreshChannel()
{
    const Settings &s = settings::get();
    lv_label_set_text_fmt(chanPage.current, "%s - %u MHz", s.chan[0] ? s.chan : "", s.freqMhz);
    const channels::Band &band = channels::BANDS[chanPage.shown];
    for (int i = 0; i < channels::BAND_COUNT; i++) setSelected(chanPage.band[i], i == chanPage.shown, true);
    for (int i = 0; i < 8; i++)
    {
        lv_label_set_text_fmt(chanPage.chanId[i], "%c%d", band.id, i + 1);
        lv_label_set_text_fmt(chanPage.chanMhz[i], "%u", band.mhz[i]);
        setSelected(chanPage.chan[i], s.chan[0] == band.id && s.chan[1] == '1' + i);
    }
    lv_label_set_text(chanPage.bandName, band.name);
}

// --- Sensitivity --------------------------------------------------------------------------

static struct
{
    lv_obj_t *chart, *preset[3], *peak;
    lv_chart_series_t *signal, *enter, *exit;
    float peakDb;
} sens;


static int getEnter() { return (int)(settings::get().detector.enterDb * 10 + 0.5f); }
static int getExit() { return (int)(settings::get().detector.exitDb * 10 + 0.5f); }
static void setEnter(int v)
{
    DetectorParams &d = settings::get().detector;
    if (v <= getExit()) v = getExit() + 5;
    d.enterDb = v / 10.0f;
    refresh(S_SENSITIVITY);
}
static void setExit(int v)
{
    DetectorParams &d = settings::get().detector;
    if (v >= getEnter()) v = getEnter() - 5;
    d.exitDb = v < 0 ? 0 : v / 10.0f;
    refresh(S_SENSITIVITY);
}
static void fmtDb(int v, char *out, size_t len) { snprintf(out, len, "+%d.%d", v / 10, v % 10); }

// dB above the receiver floor: passes reach 30-40, calibrated enter is ~22.
static Stepper enterStep = {nullptr, getEnter, setEnter, 5, 5, 450, fmtDb};
static Stepper exitStep = {nullptr, getExit, setExit, 5, 0, 445, fmtDb};

static void pickPreset(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    DetectorParams &d = settings::get().detector;
    float enter, exit;
    presets::apply((presets::Kind)i, calibration::refEnterDb(), calibration::refExitDb(), enter, exit);
    d.exitDb = 0; // order: never let enter <= exit in between
    d.enterDb = enter;
    d.exitDb = exit;
    save();
    refresh(S_SENSITIVITY);
}

static void buildSensitivity()
{
    lv_obj_t *s = screens[S_SENSITIVITY] = screen();
    header(s, LV_SYMBOL_EYE_OPEN, "Gate sensitivity", S_SETTINGS);
    sens.peak = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_align(sens.peak, LV_ALIGN_TOP_RIGHT, -16, 22);

    sens.chart = lv_chart_create(s);
    lv_obj_set_pos(sens.chart, 16, 60);
    lv_obj_set_size(sens.chart, 260, 212);
    lv_chart_set_type(sens.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(sens.chart, 120);
    lv_chart_set_update_mode(sens.chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_axis_range(sens.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 480); // dB x10 above the floor
    lv_chart_set_div_line_count(sens.chart, 0, 0);
    fill(sens.chart, col::SURFACE, col::LINE, 12);
    lv_obj_set_style_size(sens.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(sens.chart, 2, LV_PART_ITEMS);
    sens.signal = lv_chart_add_series(sens.chart, lv_color_hex(col::SOFT), LV_CHART_AXIS_PRIMARY_Y);
    sens.enter = lv_chart_add_series(sens.chart, lv_color_hex(col::ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    sens.exit = lv_chart_add_series(sens.chart, lv_color_hex(col::MUTED), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(sens.chart, sens.signal, 0);
    lv_obj_t *hint =
        text(s, &lv_font_montserrat_12, col::MUTED, "dB above the receiver floor. Med is the calibrated (or default) level.");
    lv_obj_set_pos(hint, 16, 280);
    lv_obj_set_width(hint, 260);

    lv_obj_t *seg = panel(s, 288, 60, 176, 46, col::SURFACE, col::LINE, 12);
    const char *names[] = {"Low", "Med", "High"};
    for (int i = 0; i < 3; i++)
    {
        sens.preset[i] = button(seg, 3 + i * 57, 3, 55, 38, col::SURFACE, 0, 9, pickPreset, (void *)(intptr_t)i);
        lv_obj_center(text(sens.preset[i], &lv_font_montserrat_14, col::TEXT, names[i]));
    }
    lv_obj_t *e = panel(s, 288, 114, 176, 82, col::SURFACE, col::LINE, 12);
    lv_obj_set_pos(text(e, &lv_font_montserrat_12, col::ACCENT, "Enter"), 10, 6);
    stepper(e, 2, 30, &enterStep);
    lv_obj_t *x = panel(s, 288, 204, 176, 82, col::SURFACE, col::LINE, 12);
    lv_obj_set_pos(text(x, &lv_font_montserrat_12, col::MUTED, "Exit"), 10, 6);
    stepper(x, 2, 30, &exitStep);
}

static void refreshSensitivity()
{
    const DetectorParams &d = settings::get().detector;
    for (int i = 0; i < 3; i++)
        setSelected(sens.preset[i], presetOf(d) == (presets::Kind)i, true);
    stepperRefresh(&enterStep);
    stepperRefresh(&exitStep);
    lv_chart_set_all_values(sens.chart, sens.enter, (int32_t)(d.enterDb * 10));
    lv_chart_set_all_values(sens.chart, sens.exit, (int32_t)(d.exitDb * 10));
}

static void tickSensitivity()
{
    float db = app::signalDb();
    if (db > sens.peakDb) sens.peakDb = db;
    lv_chart_set_next_value(sens.chart, sens.signal, (int32_t)(db < 0 ? 0 : db * 10));
    char b[24];
    snprintf(b, sizeof(b), "Live  -  peak %+.0f dB", sens.peakDb);
    lv_label_set_text(sens.peak, b);
}

// --- False reads --------------------------------------------------------------------------

static int getMinLap() { return (int)settings::get().detector.minLapMs; }
static void setMinLap(int v) { settings::get().detector.minLapMs = (uint32_t)v; }
static void fmtSeconds(int ms, char *out, size_t len) { snprintf(out, len, "%d.%d s", ms / 1000, ms % 1000 / 100); }
static Stepper minLapStep = {nullptr, getMinLap, setMinLap, 500, 1000, 30000, fmtSeconds};

static bool getPark() { return settings::get().ignoreParked; }
static void setPark(bool on) { settings::get().ignoreParked = on; }
static Toggle parkToggle = {nullptr, getPark, setPark};

static void buildFalseReads()
{
    lv_obj_t *s = screens[S_FALSE_READS] = screen();
    header(s, LV_SYMBOL_WARNING, "False-read protection", S_SETTINGS);
    lv_obj_t *card = panel(s, 16, 60, 448, 130, col::SURFACE, col::LINE, 16);
    rowTitle(card, 6, "Minimum lap time", "Passes sooner than this are ignored");
    stepper(card, 262, 10, &minLapStep);
    rowDivider(card, 64);
    rowTitle(card, 66, "Ignore parked quads", "A peak over 1.5 s is a quad sitting at the gate");
    toggle(card, 376, 82, &parkToggle);
    lv_obj_t *hint = text(s, &lv_font_montserrat_12, col::MUTED,
                          "Stops a crash into the gate, or a bounce back through it, from counting as a lap. "
                          "Set the minimum a few seconds under your fastest real lap.");
    lv_obj_set_pos(hint, 16, 202);
    lv_obj_set_width(hint, 448);
}

static void refreshFalseReads()
{
    stepperRefresh(&minLapStep);
    toggleRefresh(&parkToggle);
}

// --- Radio output -------------------------------------------------------------------------

static bool getBle() { return settings::get().links & LINK_BLE; }
static void setBle(bool on) { timer_settings::set(settings::get(), "ble", on ? "on" : "off"); }
static bool getEspnow() { return settings::get().links & LINK_ESPNOW; }
static void setEspnow(bool on) { timer_settings::set(settings::get(), "espnow", on ? "on" : "off"); }
static Toggle bleToggle = {nullptr, getBle, setBle};
static Toggle espnowToggle = {nullptr, getEspnow, setEspnow};

static void buildRadio()
{
    lv_obj_t *s = screens[S_RADIO] = screen();
    header(s, LV_SYMBOL_BLUETOOTH, "Radio output", S_SETTINGS);
    lv_obj_t *card = panel(s, 16, 60, 448, 180, col::SURFACE, col::LINE, 16);
    rowTitle(card, 2, "Bluetooth", "Laps to the LapDog app");
    toggle(card, 376, 16, &bleToggle);
    rowDivider(card, 60);
    rowTitle(card, 62, "ESP-NOW", "Goggles OSD and LapDog LCD (ELRS bind phrase)");
    toggle(card, 376, 76, &espnowToggle);
    rowDivider(card, 120);
    rowTitle(card, 122, "USB", "Everything, whenever it's plugged in");
    lv_obj_t *always = text(card, &lv_font_montserrat_14, col::MUTED, "Always");
    lv_obj_set_pos(always, 380, 142);
    lv_obj_t *hint = text(s, &lv_font_montserrat_12, col::MUTED,
                          "The bind phrase is set from the app or over USB. Changes apply the next time the timer arms.");
    lv_obj_set_pos(hint, 16, 252);
    lv_obj_set_width(hint, 448);
}

static void refreshRadio()
{
    toggleRefresh(&bleToggle);
    toggleRefresh(&espnowToggle);
}

// --- Race messages ------------------------------------------------------------------------

static bool getMsgEvent() { return settings::get().msg.event; }
static void setMsgEvent(bool on) { settings::get().msg.event = on; }
static bool getMsgHeat() { return settings::get().msg.heat; }
static void setMsgHeat(bool on) { settings::get().msg.heat = on; }
static bool getMsgStart() { return settings::get().msg.startFinish; }
static void setMsgStart(bool on) { settings::get().msg.startFinish = on; }
static bool getMsgLaps() { return settings::get().msg.laps; }
static void setMsgLaps(bool on) { settings::get().msg.laps = on; }
static Toggle msgToggles[4] = {
    {nullptr, getMsgEvent, setMsgEvent},
    {nullptr, getMsgHeat, setMsgHeat},
    {nullptr, getMsgStart, setMsgStart},
    {nullptr, getMsgLaps, setMsgLaps},
};
static int getNextHeat() { return settings::nextHeat(); }
static void setNextHeat(int v) { settings::setNextHeatUnsaved((uint16_t)v); }
static void fmtHeat(int v, char *out, size_t len) { snprintf(out, len, "%d", v); }
static Stepper heatStep = {nullptr, getNextHeat, setNextHeat, 1, 1, 9999, fmtHeat};
static lv_obj_t *eventValue;

static void buildMessages()
{
    lv_obj_t *s = screens[S_MESSAGES] = screen();
    header(s, LV_SYMBOL_EDIT, "Race messages", S_SETTINGS);
    lv_obj_t *card = panel(s, 16, 58, 448, 254, col::SURFACE, col::LINE, 16);
    // Event name: tap the name to type a new one.
    rowTitle(card, 0, "Event name", nullptr);
    lv_obj_t *edit = button(card, 140, 6, 222, 40, col::BG, col::LINE, 10, goTo, (void *)(intptr_t)S_KEYBOARD);
    eventValue = text(edit, &lv_font_montserrat_14, col::SELECTED_TEXT, "");
    lv_label_set_long_mode(eventValue, LV_LABEL_LONG_DOT);
    lv_obj_set_width(eventValue, 196);
    lv_obj_align(eventValue, LV_ALIGN_LEFT_MID, 10, 0);
    toggle(card, 376, 11, &msgToggles[0]);
    rowDivider(card, 52);
    rowTitle(card, 52, "Heat name", "Next heat");
    stepper(card, 190, 56, &heatStep);
    toggle(card, 376, 63, &msgToggles[1]);
    rowDivider(card, 104);
    rowTitle(card, 104, "Race start & finish", "Race mode only");
    toggle(card, 376, 115, &msgToggles[2]);
    rowDivider(card, 156);
    rowTitle(card, 156, "Lap times", "Row 3 for 3 s, each lap's time for 4 s");
    toggle(card, 376, 167, &msgToggles[3]);
    lv_obj_t *hint = text(card, &lv_font_montserrat_12, col::MUTED, "Sent with the laps over every radio output that's on.");
    lv_obj_set_pos(hint, 14, 220);
}

static void refreshMessages()
{
    lv_label_set_text(eventValue, settings::get().event[0] ? settings::get().event : "Tap to name it");
    for (Toggle &t : msgToggles) toggleRefresh(&t);
    stepperRefresh(&heatStep);
}

// --- Race defaults ------------------------------------------------------------------------

static struct
{
    lv_obj_t *byLaps, *byTime, *practice, *race;
} racePage;

static int getLaps() { return settings::get().raceLaps; }
static void setLaps(int v) { settings::get().raceLaps = (uint8_t)v; }
static void fmtLaps(int v, char *out, size_t len) { snprintf(out, len, v == 1 ? "%d lap" : "%d laps", v); }
static Stepper lapsStep = {nullptr, getLaps, setLaps, 1, 1, 99, fmtLaps};
static int getTime() { return settings::get().raceTimeS; }
static void setTime(int v) { settings::get().raceTimeS = (uint16_t)v; }
static Stepper timeStep = {nullptr, getTime, setTime, 30, 30, 3600, fmtMinutes};
static bool getHold() { return settings::get().randomHold; }
static void setHold(bool on) { settings::get().randomHold = on; }
static Toggle holdToggle = {nullptr, getHold, setHold};

static void pickLimit(lv_event_t *e)
{
    settings::get().raceLimit = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    save();
    refresh(S_RACE);
}

static void pickBootMode(lv_event_t *e)
{
    settings::get().mode = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    save();
    refresh(S_RACE);
}

static lv_obj_t *segment(lv_obj_t *card, int x, int y, int w, const char *label, lv_event_cb_t cb, int value)
{
    lv_obj_t *b = button(card, x, y, w, 38, col::SURFACE, col::LINE, 10, cb, (void *)(intptr_t)value);
    lv_obj_center(text(b, &lv_font_montserrat_14, col::TEXT, label));
    return b;
}

static void buildRace()
{
    lv_obj_t *s = screens[S_RACE] = screen();
    header(s, LV_SYMBOL_LOOP, "Race defaults", S_SETTINGS);
    lv_obj_t *card = panel(s, 16, 58, 448, 254, col::SURFACE, col::LINE, 16);
    rowTitle(card, 0, "Race ends on", nullptr);
    racePage.byLaps = segment(card, 262, 6, 84, "Laps", pickLimit, race::LIMIT_LAPS);
    racePage.byTime = segment(card, 350, 6, 84, "Time", pickLimit, race::LIMIT_TIME);
    rowDivider(card, 50);
    rowTitle(card, 50, "Lap limit", nullptr);
    stepper(card, 262, 53, &lapsStep);
    rowDivider(card, 100);
    rowTitle(card, 100, "Time limit", nullptr);
    stepper(card, 262, 103, &timeStep);
    rowDivider(card, 150);
    rowTitle(card, 150, "BOOT button arms", "Long press on the timer");
    racePage.practice = segment(card, 262, 156, 84, "Practice", pickBootMode, race::PRACTICE);
    racePage.race = segment(card, 350, 156, 84, "Race", pickBootMode, race::RACE);
    rowDivider(card, 200);
    rowTitle(card, 200, "Random start hold", "1-3 s between stage and GO");
    toggle(card, 376, 211, &holdToggle);
}

static void refreshRace()
{
    const Settings &s = settings::get();
    setSelected(racePage.byLaps, s.raceLimit == race::LIMIT_LAPS);
    setSelected(racePage.byTime, s.raceLimit == race::LIMIT_TIME);
    setSelected(racePage.practice, s.mode == race::PRACTICE);
    setSelected(racePage.race, s.mode == race::RACE);
    stepperRefresh(&lapsStep);
    stepperRefresh(&timeStep);
    toggleRefresh(&holdToggle);
}

// --- Event name keyboard ------------------------------------------------------------------

static struct
{
    lv_obj_t *area, *kb, *count;
} keys;

static void keyboardDone(lv_event_t *)
{
    if (timer_settings::set(settings::get(), "event", lv_textarea_get_text(keys.area))) save();
    show(S_MESSAGES);
}

static void keyboardCancel(lv_event_t *) { show(S_MESSAGES); }

// The goggles' font is capitals only, so the keyboard is too.
static void keyboardChanged(lv_event_t *)
{
    if (lv_keyboard_get_mode(keys.kb) == LV_KEYBOARD_MODE_TEXT_LOWER)
        lv_keyboard_set_mode(keys.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    const char *t = lv_textarea_get_text(keys.area);
    bool lower = false;
    for (const char *p = t; *p; p++) lower |= *p >= 'a' && *p <= 'z';
    if (lower)
    {
        char up[race::EVENT_MAX + 1];
        race::upper(t, up, sizeof(up));
        lv_textarea_set_text(keys.area, up);
    }
    lv_label_set_text_fmt(keys.count, "%u/%u", (unsigned)strlen(lv_textarea_get_text(keys.area)),
                          (unsigned)race::EVENT_MAX);
}

static void buildKeyboard()
{
    lv_obj_t *s = screens[S_KEYBOARD] = screen();
    lv_obj_t *x = button(s, 4, 4, 48, 48, col::BG, 0, 24, keyboardCancel);
    lv_obj_center(text(x, &lv_font_montserrat_20, col::TEXT, LV_SYMBOL_CLOSE));
    lv_obj_set_pos(text(s, &lv_font_montserrat_24, col::TEXT, "Event name"), 56, 14);
    lv_obj_t *done = button(s, 376, 10, 88, 38, col::ACCENT, 0, 10, keyboardDone);
    lv_obj_center(text(done, &lv_font_montserrat_16, col::ON_ACCENT, "DONE"));

    keys.area = lv_textarea_create(s);
    lv_obj_set_pos(keys.area, 16, 58);
    lv_obj_set_size(keys.area, 448, 44);
    lv_textarea_set_one_line(keys.area, true);
    lv_textarea_set_max_length(keys.area, race::EVENT_MAX);
    lv_obj_set_style_text_font(keys.area, &lv_font_montserrat_20, 0);
    fill(keys.area, col::SURFACE, col::ACCENT, 12);
    lv_obj_set_style_border_width(keys.area, 2, 0);
    lv_obj_set_style_text_color(keys.area, lv_color_hex(col::TEXT), 0);
    lv_obj_add_event_cb(keys.area, keyboardChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    keys.count = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_align(keys.count, LV_ALIGN_TOP_RIGHT, -26, 74);

    keys.kb = lv_keyboard_create(s);
    // LVGL aligns a keyboard to the bottom: a position would be an offset from there.
    lv_obj_set_size(keys.kb, 480, 210);
    lv_obj_align(keys.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keys.kb, keys.area);
    lv_keyboard_set_mode(keys.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_obj_set_style_bg_color(keys.kb, lv_color_hex(col::BG), 0);
    lv_obj_set_style_bg_color(keys.kb, lv_color_hex(col::SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keys.kb, lv_color_hex(col::TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keys.kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_border_color(keys.kb, lv_color_hex(col::LINE), LV_PART_ITEMS);
    lv_obj_add_event_cb(keys.kb, keyboardChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(keys.kb, keyboardDone, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keys.kb, keyboardCancel, LV_EVENT_CANCEL, nullptr);
}

static void refreshKeyboard()
{
    lv_textarea_set_text(keys.area, settings::get().event);
    keyboardChanged(nullptr);
}


// --- Website sync -------------------------------------------------------------------------
// Pair once with a code from the website, save a 2.4 GHz network, then SYNC NOW
// uploads every heat in the timer's flash that the website doesn't have (cloud.h).

static struct
{
    lv_obj_t *title, *detail, *wifiValue, *pair, *pairLabel, *sync, *syncLabel, *message;
    uint32_t unpairArmedAt; // millis() of the first tap on UNPAIR; a second within 4 s confirms
} syncPage;

static void syncNow(lv_event_t *)
{
    cloud::startSync();
    refresh(S_SYNC);
}

static void pairOrUnpair(lv_event_t *)
{
    if (!cloud::paired())
    {
        show(S_PAIR);
        return;
    }
    if (syncPage.unpairArmedAt && millis() - syncPage.unpairArmedAt < 4000)
    {
        syncPage.unpairArmedAt = 0;
        cloud::startUnpair();
    }
    else syncPage.unpairArmedAt = millis();
    refresh(S_SYNC);
}

static void buildSync()
{
    lv_obj_t *s = screens[S_SYNC] = screen();
    header(s, LV_SYMBOL_UPLOAD, "Website sync", S_SETTINGS);

    lv_obj_t *card = panel(s, 16, 60, 448, 70, col::SURFACE, col::LINE, 14);
    syncPage.title = text(card, &lv_font_montserrat_16, col::TEXT, "");
    lv_obj_set_pos(syncPage.title, 14, 12);
    lv_label_set_long_mode(syncPage.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(syncPage.title, 420);
    syncPage.detail = text(card, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_set_pos(syncPage.detail, 14, 40);
    lv_label_set_long_mode(syncPage.detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(syncPage.detail, 420);

    lv_obj_t *wifi = button(s, 16, 142, 218, 56, col::SURFACE, col::LINE, 14, goTo, (void *)(intptr_t)S_WIFI);
    lv_obj_set_pos(text(wifi, &lv_font_montserrat_16, col::TEXT, LV_SYMBOL_WIFI "  Wi-Fi"), 14, 8);
    syncPage.wifiValue = text(wifi, &lv_font_montserrat_12, col::SELECTED_TEXT, "");
    lv_obj_set_pos(syncPage.wifiValue, 14, 32);
    lv_label_set_long_mode(syncPage.wifiValue, LV_LABEL_LONG_DOT);
    lv_obj_set_width(syncPage.wifiValue, 190);

    syncPage.pair = button(s, 246, 142, 218, 56, col::SURFACE, col::LINE, 14, pairOrUnpair);
    syncPage.pairLabel = text(syncPage.pair, &lv_font_montserrat_16, col::TEXT, "");
    lv_obj_center(syncPage.pairLabel);

    syncPage.sync = button(s, 16, 210, 448, 60, col::ACCENT, 0, 16, syncNow);
    lv_obj_set_style_bg_color(syncPage.sync, lv_color_hex(0x2F7FC2), LV_STATE_PRESSED);
    syncPage.syncLabel = text(syncPage.sync, &lv_font_montserrat_24, col::ON_ACCENT, "");
    lv_obj_center(syncPage.syncLabel);

    syncPage.message = text(s, &lv_font_montserrat_12, col::SOFT, "");
    lv_obj_set_pos(syncPage.message, 16, 282);
    lv_label_set_long_mode(syncPage.message, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(syncPage.message, 448);
}

static void refreshSync()
{
    cloud::Status st = cloud::status();
    bool paired = cloud::paired();
    int waiting = cloud::pending();
    if (paired) lv_label_set_text_fmt(syncPage.title, "Paired as %s", cloud::deviceName());
    else lv_label_set_text(syncPage.title, "Not paired with the website");
    if (!cloud::storeReady()) lv_label_set_text(syncPage.detail, "Flash storage failed: finished heats aren't kept for sync");
    else if (paired)
    {
        if (waiting < 0) waiting = 0;
        const char *pilot = cloud::pilotName();
        if (pilot[0]) lv_label_set_text_fmt(syncPage.detail, "%s's account - %d heat%s waiting", pilot, waiting, waiting == 1 ? "" : "s");
        else lv_label_set_text_fmt(syncPage.detail, "%d heat%s waiting", waiting, waiting == 1 ? "" : "s");
    }
    else lv_label_set_text(syncPage.detail, "On the website: Devices > Pair a device, then Pair here");

    int nets = cloud::networkCount();
    if (nets) lv_label_set_text_fmt(syncPage.wifiValue, "%s%s", cloud::network(0).ssid, nets > 1 ? " + more" : "");
    else lv_label_set_text(syncPage.wifiValue, "No network saved");

    bool confirming = syncPage.unpairArmedAt && millis() - syncPage.unpairArmedAt < 4000;
    if (!confirming) syncPage.unpairArmedAt = 0;
    lv_label_set_text(syncPage.pairLabel, !paired ? LV_SYMBOL_PLUS "  Pair" : confirming ? "Tap again to unpair" : "Unpair");
    lv_obj_set_style_text_color(syncPage.pairLabel, lv_color_hex(confirming ? col::ALERT : col::TEXT), 0);

    bool can = paired && !st.running;
    lv_obj_set_style_bg_color(syncPage.sync, lv_color_hex(can ? col::ACCENT : col::LINE), 0);
    lv_obj_set_style_text_color(syncPage.syncLabel, lv_color_hex(can ? col::ON_ACCENT : col::MUTED), 0);
    if (st.running && st.total) lv_label_set_text_fmt(syncPage.syncLabel, "SYNCING %u/%u", st.done, st.total);
    else lv_label_set_text(syncPage.syncLabel, st.running ? "WORKING..." : LV_SYMBOL_UPLOAD "  SYNC NOW");
    lv_label_set_text(syncPage.message, st.message);
    lv_obj_set_style_text_color(syncPage.message, lv_color_hex(!st.running && st.job != cloud::JOB_NONE && !st.ok ? col::ALERT : col::SOFT), 0);
}

// --- Wi-Fi networks -----------------------------------------------------------------------

static struct
{
    lv_obj_t *saved, *nearby, *scanLabel, *note;
    int shownScans, shownSaved; // what's on screen: rebuilt only when it changes
    bool scanned;
} wifiPage;

static char wifiSsid[33]; // the network the password keyboard is for

static void startScan(lv_event_t *)
{
    wifiPage.scanned = true;
    cloud::startScan();
    refresh(S_WIFI);
}

static void forgetNetwork(lv_event_t *e)
{
    cloud::removeNetwork((int)(intptr_t)lv_event_get_user_data(e));
    wifiPage.shownSaved = -1;
    refresh(S_WIFI);
}

static void pickNetwork(lv_event_t *e)
{
    cloud::ScanResult r = cloud::scanResult((int)(intptr_t)lv_event_get_user_data(e));
    if (!r.ssid[0]) return;
    snprintf(wifiSsid, sizeof(wifiSsid), "%s", r.ssid);
    if (r.open)
    {
        if (!cloud::saveNetwork(r.ssid, "")) lv_label_set_text(wifiPage.note, "Five networks saved: forget one first");
        wifiPage.shownScans = -1;
        refresh(S_WIFI);
    }
    else show(S_WIFI_KEYS);
}

// One tappable row in a list card.
static lv_obj_t *listRow(lv_obj_t *list, int i, const char *title, const char *right, lv_event_cb_t cb, void *user)
{
    // Before the page has been on screen, LVGL hasn't laid the list out: its width would read 0.
    lv_obj_update_layout(list);
    lv_obj_t *row = button(list, 6, 6 + i * 46, lv_obj_get_width(list) - 12, 42, col::SURFACE, 0, 10, cb, user);
    lv_obj_t *t = text(row, &lv_font_montserrat_14, col::TEXT, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 10, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, lv_obj_get_width(list) - 70);
    lv_obj_t *r = text(row, &lv_font_montserrat_14, col::MUTED, right);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -10, 0);
    return row;
}

static void buildWifi()
{
    lv_obj_t *s = screens[S_WIFI] = screen();
    header(s, LV_SYMBOL_WIFI, "Wi-Fi", S_SYNC);
    lv_obj_t *scan = button(s, 352, 10, 112, 38, col::ACCENT, 0, 10, startScan);
    wifiPage.scanLabel = text(scan, &lv_font_montserrat_16, col::ON_ACCENT, LV_SYMBOL_REFRESH " SCAN");
    lv_obj_center(wifiPage.scanLabel);

    lv_obj_set_pos(text(s, &lv_font_montserrat_12, col::MUTED, "SAVED - tap to forget"), 16, 62);
    wifiPage.saved = panel(s, 16, 80, 218, 214, col::SURFACE, col::LINE, 14);
    lv_obj_set_pos(text(s, &lv_font_montserrat_12, col::MUTED, "NEARBY 2.4 GHZ - tap to add"), 246, 62);
    wifiPage.nearby = panel(s, 246, 80, 218, 214, col::SURFACE, col::LINE, 14);
    lv_obj_add_flag(wifiPage.nearby, LV_OBJ_FLAG_SCROLLABLE);
    wifiPage.note = text(s, &lv_font_montserrat_12, col::SOFT, "");
    lv_obj_set_pos(wifiPage.note, 16, 300);
    wifiPage.shownScans = wifiPage.shownSaved = -1;
}

static void refreshWifi()
{
    cloud::Status st = cloud::status();
    bool scanning = st.running && st.job == cloud::JOB_SCAN;
    lv_label_set_text(wifiPage.scanLabel, scanning ? "..." : LV_SYMBOL_REFRESH " SCAN");

    int nets = cloud::networkCount();
    if (nets != wifiPage.shownSaved)
    {
        wifiPage.shownSaved = nets;
        lv_obj_clean(wifiPage.saved);
        for (int i = 0; i < nets; i++)
            listRow(wifiPage.saved, i, cloud::network(i).ssid, LV_SYMBOL_CLOSE, forgetNetwork, (void *)(intptr_t)i);
        if (!nets) lv_obj_center(text(wifiPage.saved, &lv_font_montserrat_14, col::MUTED, "None yet"));
    }

    int n = cloud::scanCount();
    if (n != wifiPage.shownScans && !scanning)
    {
        wifiPage.shownScans = n;
        lv_obj_clean(wifiPage.nearby);
        for (int i = 0; i < n; i++)
        {
            cloud::ScanResult r = cloud::scanResult(i);
            listRow(wifiPage.nearby, i, r.ssid, r.open ? "open" : LV_SYMBOL_PLUS, pickNetwork, (void *)(intptr_t)i);
        }
        if (!n)
            lv_obj_center(text(wifiPage.nearby, &lv_font_montserrat_14, col::MUTED, wifiPage.scanned ? "None found" : "Tap SCAN"));
    }
    if (scanning || st.job == cloud::JOB_SCAN) lv_label_set_text(wifiPage.note, st.message);
}

// --- Wi-Fi password keyboard ----------------------------------------------------------------

static struct
{
    lv_obj_t *title, *area, *kb;
} passKeys;

static void styleKeyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, lv_color_hex(col::BG), 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(col::SURFACE), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(col::TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(col::LINE), LV_PART_ITEMS);
}

static lv_obj_t *keyArea(lv_obj_t *s, int maxLen)
{
    lv_obj_t *a = lv_textarea_create(s);
    lv_obj_set_pos(a, 16, 58);
    lv_obj_set_size(a, 448, 44);
    lv_textarea_set_one_line(a, true);
    lv_textarea_set_max_length(a, maxLen);
    lv_obj_set_style_text_font(a, &lv_font_montserrat_20, 0);
    fill(a, col::SURFACE, col::ACCENT, 12);
    lv_obj_set_style_border_width(a, 2, 0);
    lv_obj_set_style_text_color(a, lv_color_hex(col::TEXT), 0);
    return a;
}

static void passDone(lv_event_t *)
{
    bool ok = cloud::saveNetwork(wifiSsid, lv_textarea_get_text(passKeys.area));
    lv_label_set_text(wifiPage.note, ok ? "Saved: it's tried on the next sync" : "Five networks saved: forget one first");
    wifiPage.shownScans = -1;
    show(S_WIFI);
}

static void passCancel(lv_event_t *) { show(S_WIFI); }

static void buildWifiKeys()
{
    lv_obj_t *s = screens[S_WIFI_KEYS] = screen();
    lv_obj_t *x = button(s, 4, 4, 48, 48, col::BG, 0, 24, passCancel);
    lv_obj_center(text(x, &lv_font_montserrat_20, col::TEXT, LV_SYMBOL_CLOSE));
    passKeys.title = text(s, &lv_font_montserrat_20, col::TEXT, "");
    lv_obj_set_pos(passKeys.title, 56, 16);
    lv_label_set_long_mode(passKeys.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(passKeys.title, 310);
    lv_obj_t *done = button(s, 376, 10, 88, 38, col::ACCENT, 0, 10, passDone);
    lv_obj_center(text(done, &lv_font_montserrat_16, col::ON_ACCENT, "SAVE"));
    // Shown as typed, so a long password can be checked before it's saved.
    passKeys.area = keyArea(s, 64);
    passKeys.kb = lv_keyboard_create(s);
    lv_obj_set_size(passKeys.kb, 480, 210);
    lv_obj_align(passKeys.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(passKeys.kb, passKeys.area);
    styleKeyboard(passKeys.kb);
    lv_obj_add_event_cb(passKeys.kb, passDone, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(passKeys.kb, passCancel, LV_EVENT_CANCEL, nullptr);
}

static void refreshWifiKeys()
{
    lv_label_set_text_fmt(passKeys.title, "Password for %s", wifiSsid);
    lv_textarea_set_text(passKeys.area, "");
    lv_keyboard_set_mode(passKeys.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

// --- Pairing code ---------------------------------------------------------------------------
// Six boxes and a dash, as the website shows the code (and the app takes it).

static const char *CODE_CHARS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

static struct
{
    lv_obj_t *area, *kb, *box[6], *ch[6], *go;
} pairPage;

static void pairCancel(lv_event_t *) { show(S_SYNC); }

static void pairGo(lv_event_t *)
{
    const char *code = lv_textarea_get_text(pairPage.area);
    if (strlen(code) != 6) return;
    cloud::startPair(code);
    show(S_SYNC);
}

// Keeps the code to valid characters in capitals, and shows it in the boxes.
static void pairChanged(lv_event_t *)
{
    const char *t = lv_textarea_get_text(pairPage.area);
    char clean[7] = "";
    int n = 0;
    for (const char *p = t; *p && n < 6; p++)
    {
        char c = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        if (strchr(CODE_CHARS, c)) clean[n++] = c;
    }
    clean[n] = '\0';
    if (strcmp(clean, t)) lv_textarea_set_text(pairPage.area, clean);
    for (int i = 0; i < 6; i++)
    {
        char c[2] = {i < n ? clean[i] : '\0', '\0'};
        lv_label_set_text(pairPage.ch[i], c);
        lv_obj_set_style_border_color(pairPage.box[i], lv_color_hex(i == (n < 6 ? n : 5) ? col::ACCENT : col::LINE), 0);
    }
    lv_obj_set_style_bg_color(pairPage.go, lv_color_hex(n == 6 ? col::ACCENT : col::LINE), 0);
    if (lv_keyboard_get_mode(pairPage.kb) == LV_KEYBOARD_MODE_TEXT_LOWER)
        lv_keyboard_set_mode(pairPage.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
}

static void buildPair()
{
    lv_obj_t *s = screens[S_PAIR] = screen();
    lv_obj_t *x = button(s, 4, 4, 48, 48, col::BG, 0, 24, pairCancel);
    lv_obj_center(text(x, &lv_font_montserrat_20, col::TEXT, LV_SYMBOL_CLOSE));
    lv_obj_set_pos(text(s, &lv_font_montserrat_20, col::TEXT, "Pairing code"), 56, 8);
    lv_obj_set_pos(text(s, &lv_font_montserrat_12, col::MUTED, "Website: Devices > Pair a device"), 56, 34);
    pairPage.go = button(s, 376, 10, 88, 38, col::LINE, 0, 10, pairGo);
    lv_obj_center(text(pairPage.go, &lv_font_montserrat_16, col::ON_ACCENT, "PAIR"));

    // 6 x 52 boxes, 8 apart, with 16 more for the dash after the third: centred.
    int x0 = (480 - (6 * 52 + 5 * 8 + 16)) / 2;
    for (int i = 0; i < 6; i++)
    {
        int bx = x0 + i * 60 + (i >= 3 ? 16 : 0);
        pairPage.box[i] = panel(s, bx, 58, 52, 62, col::CODE_BOX, col::LINE, 8);
        lv_obj_set_style_border_width(pairPage.box[i], 2, 0);
        lv_obj_set_style_border_side(pairPage.box[i], LV_BORDER_SIDE_FULL, 0);
        pairPage.ch[i] = text(pairPage.box[i], &lv_font_montserrat_40, col::TEXT, "");
        lv_obj_center(pairPage.ch[i]);
    }
    panel(s, x0 + 3 * 60 - 4, 87, 12, 4, col::MUTED, 0, 2); // the dash

    // Hidden: the keyboard types into it, the boxes show it.
    pairPage.area = lv_textarea_create(s);
    lv_obj_add_flag(pairPage.area, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_one_line(pairPage.area, true);
    lv_textarea_set_max_length(pairPage.area, 6);
    lv_obj_add_event_cb(pairPage.area, pairChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    pairPage.kb = lv_keyboard_create(s);
    lv_obj_set_size(pairPage.kb, 480, 188);
    lv_obj_align(pairPage.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(pairPage.kb, pairPage.area);
    lv_keyboard_set_mode(pairPage.kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    styleKeyboard(pairPage.kb);
    lv_obj_add_event_cb(pairPage.kb, pairChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(pairPage.kb, pairGo, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(pairPage.kb, pairCancel, LV_EVENT_CANCEL, nullptr);
}

static void refreshPair()
{
    lv_textarea_set_text(pairPage.area, "");
    pairChanged(nullptr);
}

// --- History --------------------------------------------------------------------------------
// The heats kept in the board's flash (heatstore.h), newest first, a page at a time as the
// list is scrolled; a tap opens one with all its laps. Store calls wait on the store's own
// task, so a page is a few milliseconds of the LVGL task, never a flash access of its own.

static constexpr int HIST_PAGE = 8, HIST_MAX = 300, HIST_ROW = 58;

struct HistoryState
{
    heatstore::Entry entry[HIST_MAX];
    lv_obj_t *row[HIST_MAX], *sub[HIST_MAX];
    int count;
    bool more;
};

static HistoryState *hist = nullptr; // PSRAM, allocated on first use
static struct
{
    lv_obj_t *list, *info, *clear, *clearLabel, *empty;
    uint32_t askId, askAt;    // a long-pressed heat waiting for its confirming tap
    uint32_t clearAskedAt;    // Clear all waiting for its confirming tap
    uint32_t longAt;          // when a row was long-pressed (the release isn't a tap)
} histPage;

static void fmtDate(int64_t t, char *out, size_t len, bool utc)
{
    static const char *MONTH[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (t <= 0)
    {
        snprintf(out, len, "Not dated");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tmv;
    gmtime_r(&tt, &tmv); // no time zone on the board: the website's clock, UTC
    snprintf(out, len, "%d %s %02d:%02d%s", tmv.tm_mday, MONTH[tmv.tm_mon % 12], tmv.tm_hour, tmv.tm_min, utc ? " UTC" : "");
}

static bool histAsking(uint32_t id) { return histPage.askId == id && millis() - histPage.askAt < 4000; }
static bool clearAsking() { return histPage.clearAskedAt && millis() - histPage.clearAskedAt < 4000; }

static void histRowText(int i)
{
    const heatstore::Entry &e = hist->entry[i];
    lv_obj_t *row = hist->row[i];
    bool ask = histAsking(e.id);
    char d[28];
    fmtDate(e.startedAt, d, sizeof(d), false);
    char t[48];
    snprintf(t, sizeof(t), "%s  -  %s", d, ask ? "TAP AGAIN TO DELETE" : e.sent ? "uploaded" : "waiting to upload");
    lv_label_set_text(hist->sub[i], t);
    lv_obj_set_style_text_color(hist->sub[i], lv_color_hex(ask ? col::ALERT : e.sent ? col::MUTED : col::SELECTED_TEXT), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(ask ? col::ALERT : col::LINE), 0);
}

static void heatOpen(uint32_t id);
static void historyReload();

static void histRowClick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!hist || i >= hist->count || millis() - histPage.longAt < 600) return;
    uint32_t id = hist->entry[i].id;
    if (histAsking(id))
    {
        histPage.askId = 0;
        heatstore::remove(id);
        // Start the list again: the rest have moved up.
        historyReload();
        return;
    }
    heatOpen(id);
}

static void histRowLong(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!hist || i >= hist->count) return;
    histPage.longAt = millis();
    int prev = -1;
    for (int k = 0; k < hist->count; k++)
        if (hist->entry[k].id == histPage.askId) prev = k;
    histPage.askId = hist->entry[i].id;
    histPage.askAt = millis();
    if (prev >= 0) histRowText(prev);
    histRowText(i);
}

static void histAddRow(int i)
{
    const heatstore::Entry &e = hist->entry[i];
    lv_obj_t *r = button(histPage.list, 16, i * HIST_ROW, 448, HIST_ROW - 6, col::SURFACE, col::LINE, 12, histRowClick,
                         (void *)(intptr_t)i);
    lv_obj_add_event_cb(r, histRowLong, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    hist->row[i] = r;
    char t[48];
    snprintf(t, sizeof(t), "%s %u %s %s", e.prefix[0] ? e.prefix : "Heat", e.heat, "-", e.mode == race::RACE ? "Race" : "Practice");
    lv_obj_t *title = text(r, &lv_font_montserrat_16, col::TEXT, t);
    lv_obj_set_pos(title, 14, 6);
    hist->sub[i] = text(r, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_set_pos(hist->sub[i], 14, 30);
    char b[16], a[16];
    fmtLap(e.bestMs, b, sizeof(b));
    fmtLap(e.avgMs, a, sizeof(a));
    lv_obj_t *laps = text(r, &lv_font_montserrat_16, col::TEXT, "");
    lv_label_set_text_fmt(laps, "%u lap%s", e.laps, e.laps == 1 ? "" : "s");
    lv_obj_align(laps, LV_ALIGN_TOP_RIGHT, -14, 6);
    lv_obj_t *stats = text(r, &lv_font_montserrat_12, e.laps ? col::BEST : col::MUTED, "");
    if (e.laps) lv_label_set_text_fmt(stats, "best %s  avg %s", b, a);
    else lv_label_set_text(stats, "no timed laps");
    lv_obj_align(stats, LV_ALIGN_TOP_RIGHT, -14, 30);
    histRowText(i);
}

// Reads the next page into the list.
static void histLoadPage()
{
    if (!hist || !hist->more) return;
    uint32_t before = hist->count ? hist->entry[hist->count - 1].id : 0;
    int want = HIST_PAGE < HIST_MAX - hist->count ? HIST_PAGE : HIST_MAX - hist->count;
    int n = want > 0 ? heatstore::list(before, hist->entry + hist->count, want) : 0;
    for (int i = 0; i < n; i++) histAddRow(hist->count + i);
    hist->count += n;
    hist->more = n == want && hist->count < HIST_MAX;
    lv_obj_set_style_text_opa(histPage.empty, hist->count ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

static void historyReload()
{
    if (!hist)
    {
        hist = static_cast<HistoryState *>(heap_caps_calloc(1, sizeof(HistoryState), MALLOC_CAP_SPIRAM));
        if (!hist) return;
    }
    lv_obj_clean(histPage.list);
    lv_obj_scroll_to_y(histPage.list, 0, LV_ANIM_OFF);
    hist->count = 0;
    hist->more = heatstore::ready();
    histPage.askId = 0;
    histPage.clearAskedAt = 0;
    histLoadPage();
    histPage.askAt = 0;
}

static void histScrolled(lv_event_t *)
{
    if (lv_obj_get_scroll_bottom(histPage.list) < 120) histLoadPage();
}

static void clearAllTap(lv_event_t *)
{
    if (!heatstore::ready()) return;
    if (clearAsking())
    {
        histPage.clearAskedAt = 0;
        heatstore::removeAll();
        historyReload();
        return;
    }
    histPage.clearAskedAt = millis();
}

static void openHistory(lv_event_t *)
{
    if (app::armed()) return;
    show(S_HISTORY);
    historyReload();
}

static void buildHistory()
{
    lv_obj_t *s = screens[S_HISTORY] = screen();
    header(s, LV_SYMBOL_LIST, "History", S_IDLE);
    histPage.info = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_set_pos(histPage.info, 16, 62);
    lv_label_set_long_mode(histPage.info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(histPage.info, 330);
    histPage.clear = button(s, 372, 54, 92, 28, col::SURFACE, col::LINE, 14, clearAllTap);
    histPage.clearLabel = text(histPage.clear, &lv_font_montserrat_12, col::TEXT, "Clear all");
    lv_obj_center(histPage.clearLabel);
    histPage.list = lv_obj_create(s);
    lv_obj_remove_style_all(histPage.list);
    lv_obj_set_pos(histPage.list, 0, 90);
    lv_obj_set_size(histPage.list, board::WIDTH, board::HEIGHT - 90);
    lv_obj_add_flag(histPage.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(histPage.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(histPage.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(histPage.list, histScrolled, LV_EVENT_SCROLL, nullptr);
    histPage.empty = text(s, &lv_font_montserrat_16, col::MUTED, "No heats yet - they appear here once flown");
    lv_obj_align(histPage.empty, LV_ALIGN_CENTER, 0, 30);
}

static void refreshHistory()
{
    heatstore::Usage u = heatstore::usage();
    bool ask = clearAsking();
    if (!heatstore::ready()) lv_label_set_text(histPage.info, "Flash storage isn't available");
    else if (ask)
        lv_label_set_text_fmt(histPage.info, "Delete all %d heats%s?", u.waiting + u.sent,
                              u.waiting ? " (some not uploaded yet)" : "");
    else
        lv_label_set_text_fmt(histPage.info, "%d heat%s  -  %d waiting to upload  -  %.1f MB free", u.waiting + u.sent,
                              u.waiting + u.sent == 1 ? "" : "s", u.waiting, u.freeBytes / 1048576.0f);
    lv_obj_set_style_text_color(histPage.info, lv_color_hex(ask ? col::ALERT : heatstore::nearlyFull() ? col::ALERT : col::MUTED), 0);
    lv_label_set_text(histPage.clearLabel, ask ? "Tap to confirm" : "Clear all");
    lv_obj_set_style_text_color(histPage.clearLabel, lv_color_hex(ask ? col::ALERT : col::TEXT), 0);
    lv_obj_set_width(histPage.clear, ask ? 120 : 92);
    lv_obj_set_x(histPage.clear, ask ? 344 : 372);
    if (hist)
        for (int i = 0; i < hist->count; i++)
            if (hist->entry[i].id == histPage.askId) histRowText(i);
    // A confirmation that ran out reverts on its own.
    if (hist && histPage.askId && !histAsking(histPage.askId))
    {
        for (int i = 0; i < hist->count; i++) histRowText(i);
        histPage.askId = 0;
    }
}

// --- One heat -------------------------------------------------------------------------------
// Header, summary and every lap with its gap to the best: the best lap in purple, the slowest in
// the app's slow-lap red.

static heatlog::Heat *detail = nullptr; // PSRAM
static struct
{
    lv_obj_t *title, *line, *stat[5], *list;
} heatPage;

static void buildHeat()
{
    lv_obj_t *s = screens[S_HEAT] = screen();
    header(s, nullptr, "", S_HISTORY);
    heatPage.title = text(s, &lv_font_montserrat_20, col::TEXT, "");
    lv_obj_set_pos(heatPage.title, 56, 16);
    heatPage.line = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_set_pos(heatPage.line, 16, 60);
    lv_label_set_long_mode(heatPage.line, LV_LABEL_LONG_DOT);
    lv_obj_set_width(heatPage.line, 448);
    const char *names[5] = {"Laps", "Best", "Average", "Total", "Ignored"};
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *c = panel(s, 16 + i * 90, 80, 86, 50, col::SURFACE, col::LINE, 10);
        lv_obj_t *n = text(c, &lv_font_montserrat_12, col::MUTED, names[i]);
        lv_obj_set_pos(n, 8, 5);
        heatPage.stat[i] = text(c, &lv_font_montserrat_16, i == 1 ? col::BEST : col::TEXT, "");
        lv_obj_set_pos(heatPage.stat[i], 8, 25);
    }
    heatPage.list = lv_obj_create(s);
    lv_obj_remove_style_all(heatPage.list);
    lv_obj_set_pos(heatPage.list, 0, 138);
    lv_obj_set_size(heatPage.list, board::WIDTH, board::HEIGHT - 138);
    lv_obj_add_flag(heatPage.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(heatPage.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(heatPage.list, LV_SCROLLBAR_MODE_OFF);
}

static void heatShow(const heatlog::Heat &h)
{
    // heatlog::label's middle dot isn't in the UI fonts: a dash.
    char title[64];
    snprintf(title, sizeof(title), "%s %u - %s", h.prefix[0] ? h.prefix : "Heat", h.heat, h.mode == race::RACE ? "Race" : "Practice");
    lv_label_set_text(heatPage.title, title);

    char d[28], l[120];
    fmtDate(h.startedAt, d, sizeof(d), true);
    if (h.mode == race::RACE)
    {
        char r[24] = "";
        if (h.raceMs >= 0) fmtLap((uint32_t)h.raceMs, r, sizeof(r));
        if (h.lapLimit) snprintf(l, sizeof(l), "%s  -  first to %u laps%s%s", d, h.lapLimit, r[0] ? "  -  finished in " : "", r);
        else snprintf(l, sizeof(l), "%s  -  %u s race%s%s", d, h.timeLimitS, r[0] ? "  -  finished in " : "", r);
    }
    else snprintf(l, sizeof(l), "%s  -  practice%s%s", d, h.event[0] ? "  -  " : "", h.event);
    lv_label_set_text(heatPage.line, l);

    uint32_t best = 0, slow = 0;
    uint64_t sum = 0;
    int bi = -1, si = -1;
    for (int i = 0; i < h.lapCount; i++)
    {
        sum += h.laps[i].ms;
        if (bi < 0 || h.laps[i].ms < best) best = h.laps[i].ms, bi = i;
        if (si < 0 || h.laps[i].ms > slow) slow = h.laps[i].ms, si = i;
    }
    char b[16];
    lv_label_set_text_fmt(heatPage.stat[0], "%u", h.lapCount);
    fmtLap(best, b, sizeof(b));
    lv_label_set_text(heatPage.stat[1], h.lapCount ? b : "-");
    fmtLap(h.lapCount ? (uint32_t)(sum / h.lapCount) : 0, b, sizeof(b));
    lv_label_set_text(heatPage.stat[2], h.lapCount ? b : "-");
    int64_t total = h.raceMs >= 0 ? h.raceMs : (h.holeshotMs >= 0 ? h.holeshotMs + (int64_t)sum : (int64_t)sum);
    char tt[16];
    fmtLap((uint32_t)total, tt, sizeof(tt));
    lv_label_set_text(heatPage.stat[3], h.lapCount ? tt : "-");
    lv_label_set_text_fmt(heatPage.stat[4], "%u", h.ignoredSoon + h.ignoredLong);

    lv_obj_clean(heatPage.list);
    lv_obj_scroll_to_y(heatPage.list, 0, LV_ANIM_OFF);
    for (int i = 0; i < h.lapCount; i++)
    {
        bool isBest = i == bi, isSlow = i == si && h.lapCount > 2 && i != bi;
        int y = i * 34;
        lv_obj_t *r = panel(heatPage.list, 16, y, 448, 30, isBest ? col::BEST_TINT : isSlow ? 0x2A1416 : col::SURFACE,
                            isBest ? col::BEST_EDGE : isSlow ? 0x4A2226 : col::LINE, 8);
        uint32_t color = isBest ? col::BEST : isSlow ? col::ALERT : col::TEXT;
        lv_obj_t *n = text(r, &lv_font_montserrat_14, col::MUTED, "");
        lv_label_set_text_fmt(n, "Lap %u", h.laps[i].lap);
        lv_obj_set_pos(n, 12, 6);
        fmtLap(h.laps[i].ms, b, sizeof(b));
        lv_obj_t *tm = text(r, &lv_font_montserrat_16, color, b);
        lv_obj_set_pos(tm, 150, 5);
        if (!isBest)
        {
            char g[20];
            uint32_t gap = h.laps[i].ms - best;
            snprintf(g, sizeof(g), "+%lu.%03lu", (unsigned long)(gap / 1000), (unsigned long)(gap % 1000));
            lv_obj_t *gl = text(r, &lv_font_montserrat_14, isSlow ? col::ALERT : col::MUTED, g);
            lv_obj_align(gl, LV_ALIGN_TOP_RIGHT, -14, 6);
        }
        else
        {
            lv_obj_t *bl = text(r, &lv_font_montserrat_12, col::BEST, "best");
            lv_obj_align(bl, LV_ALIGN_TOP_RIGHT, -14, 8);
        }
    }
    if (!h.lapCount)
    {
        lv_obj_t *none = text(heatPage.list, &lv_font_montserrat_14, col::MUTED, "No timed laps in this heat");
        lv_obj_set_pos(none, 16, 8);
    }
}

static void heatOpen(uint32_t id)
{
    if (!detail) detail = static_cast<heatlog::Heat *>(heap_caps_malloc(sizeof(heatlog::Heat), MALLOC_CAP_SPIRAM));
    if (!detail || !heatstore::loadAny(id, *detail)) return;
    heatShow(*detail);
    show(S_HEAT);
}

static void openNewest(lv_event_t *)
{
    if (app::armed()) return;
    heatstore::Entry e;
    if (heatstore::ready() && heatstore::list(0, &e, 1) == 1) heatOpen(e.id);
    else openHistory(nullptr);
}

static void refreshHeat() {}

// --- Navigation ---------------------------------------------------------------------------

static void refresh(Screen s)
{
    switch (s)
    {
    case S_IDLE: refreshIdle(); break;
    case S_LIVE: refreshLive(); break;
    case S_SETTINGS: refreshSettings(); break;
    case S_CHANNEL: refreshChannel(); break;
    case S_SENSITIVITY: refreshSensitivity(); break;
    case S_FALSE_READS: refreshFalseReads(); break;
    case S_RADIO: refreshRadio(); break;
    case S_MESSAGES: refreshMessages(); break;
    case S_RACE: refreshRace(); break;
    case S_HISTORY: refreshHistory(); break;
    case S_HEAT: refreshHeat(); break;
    case S_KEYBOARD: refreshKeyboard(); break;
    case S_SYNC: refreshSync(); break;
    case S_WIFI: refreshWifi(); break;
    case S_WIFI_KEYS: refreshWifiKeys(); break;
    case S_PAIR: refreshPair(); break;
    default: break;
    }
}

static void show(Screen s)
{
    // Settings only while idle (the console has the same rule).
    if (s != S_LIVE && s != S_IDLE && app::armed()) s = S_LIVE;
    if (s == S_CHANNEL) chanPage.shown = bandOf(settings::get());
    if (s == S_SENSITIVITY)
    {
        sens.peakDb = 0;
        lv_chart_set_all_values(sens.chart, sens.signal, 0);
    }
    app::setLiveSignal(s == S_SENSITIVITY);
    current = s;
    refresh(s);
    lv_screen_load(screens[s]);
}

static void tick(lv_timer_t *)
{
    if (splash::active()) return; // it loads the first screen itself
    static uint32_t lastPower = 0;
    if (!lastPower || millis() - lastPower >= 5000) // the AXP2101: an I2C read, a few times a minute
    {
        lastPower = millis();
        power::poll();
    }
    bool armed = app::armed();
    if (armed && current != S_LIVE) show(S_LIVE);
    else if (!armed && current == S_LIVE) show(S_IDLE);
    else if (current == S_LIVE) refreshLive();
    else if (current == S_SENSITIVITY) tickSensitivity();
    else if (current == S_SYNC || current == S_WIFI || current == S_SETTINGS || current == S_HISTORY)
    {
        static uint32_t last = 0;
        if (millis() - last >= 250) // a sync's progress, scan results
        {
            last = millis();
            refresh(current);
        }
    }
    else if (current == S_IDLE)
    {
        static uint32_t last = 0;
        if (millis() - last >= 1000) // heat numbers and settings changed over USB
        {
            last = millis();
            refreshIdle();
        }
    }
}

// The first screen, at the splash's hand-over (live if a heat was armed meanwhile).
static void enter() { show(app::armed() ? S_LIVE : S_IDLE); }

void display::begin()
{
    if (!lv_display_get_default()) return; // no panel: display::splash() said so
    power::begin();
    // One screen at a time, letting go of LVGL in between: building them all takes
    // ~0.4 s, and the splash keeps barking meanwhile.
    void (*const builders[])() = {buildIdle,  buildLive,  buildSettings, buildChannel, buildSensitivity,
                                  buildFalseReads, buildRadio, buildMessages, buildRace, buildHistory, buildHeat, buildKeyboard,
                                  buildSync, buildWifi, buildWifiKeys, buildPair};
    for (auto build : builders)
    {
        board::lock();
        build();
        board::unlock();
    }
    board::lock();
    if (splash::active()) splash::onFinished(enter);
    else enter();
    lv_timer_create(tick, UI_TICK_MS, nullptr);
    board::unlock();
    ESP_LOGI(TAG, "touch screens up");
}

#endif // LAPDOG_TOUCH
