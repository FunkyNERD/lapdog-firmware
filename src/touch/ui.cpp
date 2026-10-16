// The touch timer's screens (Waveshare ESP32-C5-Touch-LCD-3.5, 480x320), after
// the "LapDog Touch Timer" design: idle (arm), live timing, a settings grid
// and one page per category. Everything the pilot changes here is a timer
// setting, saved to NVS straight away - the same settings the app and the USB
// console change. Settings are only reachable while idle; arming (from here,
// the BOOT button or USB) always brings up the live screen.
//
// display::frame() runs on the timing loop: it only updates a small model under
// a spinlock. LVGL's own task redraws from that model every UI_TICK_MS.

#if LAPDOG_TOUCH

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "art.h"
#include "board.h"
#include "channels.h"
#include "config.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "race.h"
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
    S_KEYBOARD,
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

// --- Idle -------------------------------------------------------------------------------

static struct
{
    lv_obj_t *chan, *radio, *event, *heat, *practice, *race, *raceLabel, *armHint;
    lv_obj_t *lastTitle, *lastLaps, *lastBest, *lastAvg;
} idle;

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
    chip(s, &idle.chan, 248, 110);
    chip(s, &idle.radio, 364, 60);
    lv_obj_t *gear = button(s, 428, 4, 48, 48, col::BG, 0, 24, goTo, (void *)(intptr_t)S_SETTINGS);
    lv_obj_center(text(gear, &lv_font_montserrat_24, col::TEXT, LV_SYMBOL_SETTINGS));

    idle.event = text(s, &lv_font_montserrat_12, col::MUTED, "");
    lv_obj_set_pos(idle.event, 16, 58);
    idle.heat = text(s, &lv_font_montserrat_48, col::TEXT, "");
    lv_obj_set_pos(idle.heat, 14, 74);

    lv_obj_t *seg = panel(s, 16, 136, 284, 50, col::SURFACE, col::LINE, 12);
    idle.practice = button(seg, 4, 4, 136, 40, col::SURFACE, 0, 9, pickMode, (void *)(intptr_t)race::PRACTICE);
    lv_obj_center(text(idle.practice, &lv_font_montserrat_16, col::TEXT, "Practice"));
    idle.race = button(seg, 142, 4, 136, 40, col::SURFACE, 0, 9, pickMode, (void *)(intptr_t)race::RACE);
    idle.raceLabel = text(idle.race, &lv_font_montserrat_16, col::TEXT, "Race");
    lv_obj_center(idle.raceLabel);

    lv_obj_t *card = panel(s, 16, 240, 284, 64, col::SURFACE, col::LINE, 12);
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
    char r[24] = "";
    if (s.links & LINK_BLE) strcat(r, LV_SYMBOL_BLUETOOTH);
    if (s.links & LINK_ESPNOW) strcat(r, s.links & LINK_BLE ? " " LV_SYMBOL_WIFI : LV_SYMBOL_WIFI);
    lv_label_set_text(idle.radio, r[0] ? r : LV_SYMBOL_USB);

    char ev[race::EVENT_MAX + 1];
    race::upper(s.event, ev, sizeof(ev));
    lv_label_set_text(idle.event, s.msg.event ? ev : "");
    lv_label_set_text_fmt(idle.heat, "%s %u", s.heatPrefix[0] ? s.heatPrefix : "Heat", settings::nextHeat());

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
    lv_label_set_text(idle.armHint, race ? "stage, then GO" : "first pass starts lap 1");

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
    lv_obj_t *status, *chan, *lapNo, *lapWord, *clock, *last, *best, *bestTitle;
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
    lv_bar_set_range(live.signal, 0, 320);
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

static struct
{
    lv_obj_t *value[6];
} grid;

static void buildSettings()
{
    lv_obj_t *s = screens[S_SETTINGS] = screen();
    header(s, nullptr, "Settings", S_IDLE);
    lv_obj_t *fw = text(s, &lv_font_montserrat_12, col::MUTED, "Firmware " FIRMWARE_VERSION " - saved as you change them");
    lv_obj_align(fw, LV_ALIGN_TOP_RIGHT, -16, 22);
    struct Tile
    {
        const char *icon, *title;
        Screen to;
    };
    static const Tile tiles[] = {
        {LV_SYMBOL_WIFI, "Channel", S_CHANNEL},           {LV_SYMBOL_EYE_OPEN, "Sensitivity", S_SENSITIVITY},
        {LV_SYMBOL_WARNING, "False reads", S_FALSE_READS}, {LV_SYMBOL_BLUETOOTH, "Radio output", S_RADIO},
        {LV_SYMBOL_EDIT, "Race messages", S_MESSAGES},    {LV_SYMBOL_LOOP, "Race defaults", S_RACE},
    };
    for (int i = 0; i < 6; i++)
    {
        int x = 16 + (i % 3) * 152, y = 60 + (i / 3) * 128;
        lv_obj_t *t = button(s, x, y, 144, 120, col::SURFACE, col::LINE, 16, goTo, (void *)(intptr_t)tiles[i].to);
        lv_obj_t *ic = panel(t, 12, 12, 36, 36, col::SELECTED, 0, 10);
        lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(text(ic, &lv_font_montserrat_20, col::ACCENT, tiles[i].icon));
        lv_obj_set_pos(text(t, &lv_font_montserrat_16, col::TEXT, tiles[i].title), 12, 70);
        grid.value[i] = text(t, &lv_font_montserrat_12, col::SELECTED_TEXT, "");
        lv_obj_set_pos(grid.value[i], 12, 92);
        lv_label_set_long_mode(grid.value[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(grid.value[i], 122);
    }
}

static const char *sensitivityName(const DetectorParams &d)
{
    if (d.enterDb == 14 && d.exitDb == 8) return "Low";
    if (d.enterDb == 10 && d.exitDb == 5) return "Medium";
    if (d.enterDb == 6 && d.exitDb == 3) return "High";
    return "Custom";
}

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

static constexpr float PRESETS[3][2] = {{14, 8}, {10, 5}, {6, 3}};

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

static Stepper enterStep = {nullptr, getEnter, setEnter, 5, 5, 300, fmtDb};
static Stepper exitStep = {nullptr, getExit, setExit, 5, 0, 295, fmtDb};

static void pickPreset(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    DetectorParams &d = settings::get().detector;
    d.exitDb = 0; // order: never let enter <= exit in between
    d.enterDb = PRESETS[i][0];
    d.exitDb = PRESETS[i][1];
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
    lv_chart_set_axis_range(sens.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 320);
    lv_chart_set_div_line_count(sens.chart, 0, 0);
    fill(sens.chart, col::SURFACE, col::LINE, 12);
    lv_obj_set_style_size(sens.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(sens.chart, 2, LV_PART_ITEMS);
    sens.signal = lv_chart_add_series(sens.chart, lv_color_hex(col::SOFT), LV_CHART_AXIS_PRIMARY_Y);
    sens.enter = lv_chart_add_series(sens.chart, lv_color_hex(col::ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    sens.exit = lv_chart_add_series(sens.chart, lv_color_hex(col::MUTED), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(sens.chart, sens.signal, 0);
    lv_obj_t *hint =
        text(s, &lv_font_montserrat_12, col::MUTED, "Fly a few passes: enter just under the peaks, exit above the floor.");
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
        setSelected(sens.preset[i], d.enterDb == PRESETS[i][0] && d.exitDb == PRESETS[i][1], true);
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
    lv_obj_set_pos(keys.kb, 0, 110);
    lv_obj_set_size(keys.kb, 480, 210);
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
    case S_KEYBOARD: refreshKeyboard(); break;
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
    bool armed = app::armed();
    if (armed && current != S_LIVE) show(S_LIVE);
    else if (!armed && current == S_LIVE) show(S_IDLE);
    else if (current == S_LIVE) refreshLive();
    else if (current == S_SENSITIVITY) tickSensitivity();
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
    // One screen at a time, letting go of LVGL in between: building them all takes
    // ~0.4 s, and the splash keeps barking meanwhile.
    void (*const builders[])() = {buildIdle,  buildLive,  buildSettings, buildChannel, buildSensitivity,
                                  buildFalseReads, buildRadio, buildMessages, buildRace, buildKeyboard};
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
