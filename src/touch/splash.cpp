// Boot splash for the touch timer, after LapDog LCD's (lapdog-lcd src/boot_splash.cpp):
// the dog mark with its signal arcs lighting in turn (a bark every BARK_MS), the
// wordmark, what this is, and a progress bar with the start-up step under way.
//
// It's up as soon as the panel is: display::splash() is the first thing app_main
// does. The start-up then reports its steps (display::bootStep()) from its own
// task and never waits on the splash; everything here runs from a timer in
// LVGL's task. The bar is paced so the splash lasts at least MIN_MS even when
// start-up is quicker, and simply follows it when it's slower. Once READY has
// had its moment the backlight fades out, the main screen is loaded and drawn,
// and the backlight fades back up on it. A tap at READY skips the rest of the hold.

#if LAPDOG_TOUCH

#include "splash.h"

#include <math.h>
#include <stdio.h>

#include "app.h"
#include "art.h"
#include "board.h"
#include "bootlog.h"
#include "channels.h"
#include "config.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"
#include "theme.h"

static const char *TAG = "splash";

namespace
{

constexpr uint32_t ARC_OFF = 0x242C3A; // a signal arc between barks
constexpr uint32_t TRACK = 0x20262F;   // the empty progress bar

// Timing. MIN_MS counts from the splash lighting up.
constexpr uint32_t TICK_MS = 20;
constexpr uint32_t MIN_MS = 1800;
constexpr uint32_t READY_HOLD_MS = 350;
constexpr uint32_t FADE_IN_MS = 250, FADE_OUT_MS = 220, REVEAL_MS = 300;
constexpr uint32_t BAR_EASE_MS = 90; // the bar's catch-up time constant

// Start-up steps reported by app_main, plus READY.
constexpr int WORK_STEPS = 4;
constexpr int TOTAL_STEPS = WORK_STEPS + 1;

// One bark: the arcs light from the inside out, hold, then go dark again.
constexpr uint32_t BARK_MS = 1200, ARC_STEP_MS = 140, BARK_HOLD_MS = 800;
constexpr uint32_t ARC_RISE_MS = 70, ARC_FALL_MS = 180;

// --- Layout (480x320) ---------------------------------------------------------------
constexpr int W = 480, H = 320;
constexpr int LOGO_Y = 32;
constexpr int WORD_Y = 146;
constexpr int SUB_Y = 196;
constexpr int BAR_W = 300, BAR_H = 4, BAR_X = (W - BAR_W) / 2, BAR_Y = 272;
constexpr int STEP_Y = BAR_Y - 22;
constexpr int FOOT_Y = H - 24;

// --- State --------------------------------------------------------------------------------
// Written by app_main, read by the timer: single core, word-sized, set before published.
volatile int stepIndex = -1;
const char *volatile stepLabel = "";
volatile bool workDone = false;
volatile uint32_t litAtMs = 0; // first frame in (0 = not yet)

enum Phase
{
    WAITING, // for the first frame
    RUNNING, // fading in, then the steps
    LEAVING, // backlight fading out
    REVEALING,
    FINISHED,
};
Phase phase = WAITING;
bool up = false;
bool skip = false;
void (*loadMain)() = nullptr;

lv_obj_t *scr, *arcs[3], *barFill, *stepText, *countText, *chanRow;
lv_timer_t *timer;
float barShown = 0;
int stepShown = -2;
bool readyShown = false;
uint32_t readyAtMs, phaseAtMs, lastTickMs;
uint8_t arcLevel[3] = {1, 1, 1}; // forces the first paint

uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// Backlight on a perceptual curve: linear PWM looks like it jumps at the bottom.
void light(float k) { board::backlight((int)lrintf(100 * k * k)); }

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int spacing, const char *s)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, spacing, 0);
    lv_label_set_text(l, s);
    return l;
}

// "A · B" as one centred-able row: the Montserrat build has no middle dot, so it's a circle.
lv_obj_t *dotted(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int spacing, int gap, const char *a,
                 const char *b)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    label(row, font, color, spacing, a);
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 4, 4);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_translate_y(dot, -1, 0); // on the capitals' middle, not the line's
    label(row, font, color, spacing, b);
    return row;
}

lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// One signal arc, centred just ahead of the nose, +-48 degrees, round-capped.
lv_obj_t *signalArc(lv_obj_t *logo, float cx, float cy, float r, int width)
{
    int d = (int)lrintf(2 * r + width);
    lv_obj_t *a = lv_arc_create(logo);
    lv_obj_remove_style_all(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, (int)lrintf(cx - d / 2.0f), (int)lrintf(cy - d / 2.0f));
    lv_arc_set_bg_angles(a, 312, 48); // LVGL's angles run clockwise from 3 o'clock, as y-down
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(ARC_OFF), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return a;
}

// --- Per-tick updates ------------------------------------------------------------------------

// How lit arc i is (0-255) at time t into the splash.
uint8_t arcLight(int i, uint32_t t)
{
    uint32_t p = t % BARK_MS, on = i * ARC_STEP_MS;
    float k;
    if (p < on) k = 0;
    else if (p < on + ARC_RISE_MS) k = (float)(p - on) / ARC_RISE_MS;
    else if (p < BARK_HOLD_MS) k = 1;
    else k = 1 - clamp01((float)(p - BARK_HOLD_MS) / ARC_FALL_MS);
    return (uint8_t)lrintf(255 * k * (2 - k)); // ease out
}

void paintArcs(uint32_t t)
{
    for (int i = 0; i < 3; i++)
    {
        uint8_t v = arcLight(i, t);
        if (v == arcLevel[i]) continue;
        arcLevel[i] = v;
        lv_obj_set_style_arc_color(arcs[i], lv_color_mix(lv_color_hex(col::ACCENT), lv_color_hex(ARC_OFF), v),
                                   LV_PART_MAIN);
    }
}

void paintStep(int i)
{
    char count[8];
    snprintf(count, sizeof(count), "%d/%d", i + 1, TOTAL_STEPS);
    lv_label_set_text(countText, count);
    lv_label_set_text_static(stepText, (const char *)stepLabel);
}

// The saved channel, once settings are loaded (they are by the second step).
void paintChannel()
{
    const Settings &s = settings::get();
    char freq[16];
    snprintf(freq, sizeof(freq), "%u MHz", s.freqMhz);
    chanRow = dotted(scr, &lv_font_montserrat_12, col::CHEVRON, 1, 6, s.chan[0] ? s.chan : "CH", freq);
    lv_obj_update_layout(chanRow);
    lv_obj_set_pos(chanRow, W - 16 - lv_obj_get_width(chanRow), FOOT_Y);
}

void finish()
{
    phase = FINISHED;
    lv_timer_delete(timer);
    timer = nullptr;
    lv_obj_delete(scr); // the main screen is loaded by now
    scr = nullptr;
    bootlog::mark("splash_end");
}

void tick(lv_timer_t *)
{
    uint32_t now = millis();
    uint32_t dt = now - lastTickMs;
    lastTickMs = now;
    if (phase == WAITING)
    {
        if (!litAtMs) return;
        phase = RUNNING;
        phaseAtMs = litAtMs;
    }
    uint32_t t = now - litAtMs;
    paintArcs(t);

    if (phase == RUNNING)
    {
        if (t < FADE_IN_MS + TICK_MS) light(clamp01((float)t / FADE_IN_MS));

        int i = stepIndex;
        if (i != stepShown && i >= 0 && !readyShown)
        {
            stepShown = i;
            paintStep(i);
            if (i >= 1 && !chanRow) paintChannel();
        }

        // Fill to the steps done so far, but no faster than the minimum time allows.
        bool done = workDone;
        float reached = done ? 1.0f : (float)(i < 0 ? 0 : i) / (TOTAL_STEPS - 1);
        float paced = (float)t / (MIN_MS - READY_HOLD_MS);
        float target = reached < paced ? reached : paced;
        barShown += (target - barShown) * clamp01((float)dt / BAR_EASE_MS);
        if (done && target >= 1 && barShown > 0.995f) barShown = 1;
        lv_obj_set_width(barFill, (int)lrintf(BAR_W * barShown));

        if (barShown >= 1 && !readyShown)
        {
            readyShown = true;
            readyAtMs = now;
            skip = false; // only taps on READY count
            lv_obj_set_style_bg_color(barFill, lv_color_hex(col::OK), 0);
            lv_label_set_text_static(stepText, "READY");
            lv_obj_set_style_text_color(stepText, lv_color_hex(col::OK), 0);
            lv_label_set_text_fmt(countText, "%d/%d", TOTAL_STEPS, TOTAL_STEPS);
            if (!chanRow) paintChannel();
            bootlog::mark("splash_ready");
        }
        // Armed already (BOOT button or USB): get the live screen up now.
        bool held = readyShown && now - readyAtMs >= READY_HOLD_MS && t >= MIN_MS;
        if (readyShown && (held || skip || app::armed()))
        {
            phase = LEAVING;
            phaseAtMs = now;
        }
    }
    else if (phase == LEAVING)
    {
        float k = clamp01((float)(now - phaseAtMs) / FADE_OUT_MS);
        light(1 - k);
        if (k >= 1)
        {
            if (loadMain) loadMain();
            lv_refr_now(nullptr); // drawn while dark, so it fades up whole
            phase = REVEALING;
            phaseAtMs = millis();
        }
    }
    else if (phase == REVEALING)
    {
        float k = clamp01((float)(now - phaseAtMs) / REVEAL_MS);
        light(k);
        if (k >= 1) finish();
    }
}

void tapped(lv_event_t *) { skip = true; }

void firstFrame()
{
    uint32_t now = millis();
    litAtMs = now ? now : 1;
}

void build()
{
    scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(col::BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, tapped, LV_EVENT_PRESSED, nullptr);

    // The mark: dog image, arcs drawn over its right.
    const float S = art::SCALE;
    const int LOGO_W = (int)lrintf((131 - art::ORIGIN_X) * S); // the arcs reach past the dog, to x = 131 units
    lv_obj_t *logo = lv_obj_create(scr);
    lv_obj_remove_style_all(logo);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(logo, LOGO_W, art::dog.header.h);
    lv_obj_set_pos(logo, (W - LOGO_W) / 2, LOGO_Y);
    lv_obj_t *dog = lv_image_create(logo);
    lv_image_set_src(dog, &art::dog);
    lv_obj_set_pos(dog, 0, 0);
    const float cx = (100 - art::ORIGIN_X) * S, cy = (56 - art::ORIGIN_Y) * S;
    static const float RADII[3] = {11, 19, 27};
    for (int i = 0; i < 3; i++) arcs[i] = signalArc(logo, cx, cy, RADII[i] * S, (int)lrintf(5 * S));

    lv_obj_t *word = lv_image_create(scr);
    lv_image_set_src(word, &art::wordmark);
    lv_obj_set_pos(word, (W - (int)art::wordmark.header.w) / 2, WORD_Y);

    lv_obj_t *sub = dotted(scr, &lv_font_montserrat_12, col::MUTED, 3, 12, "LAPDOG TIMER", "SITS BY THE GATE");
    lv_obj_update_layout(sub);
    lv_obj_set_pos(sub, (W - lv_obj_get_width(sub)) / 2, SUB_Y);

    // Progress: the step under way and the count above the bar.
    stepText = label(scr, &lv_font_montserrat_12, col::SOFT, 1, "");
    lv_obj_set_pos(stepText, BAR_X, STEP_Y);
    countText = label(scr, &lv_font_montserrat_12, col::CHEVRON, 1, "");
    lv_obj_set_width(countText, 60);
    lv_obj_set_style_text_align(countText, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(countText, BAR_X + BAR_W - 60, STEP_Y);
    box(scr, BAR_X, BAR_Y, BAR_W, BAR_H, TRACK);
    barFill = box(scr, BAR_X, BAR_Y, 0, BAR_H, col::ACCENT);

    lv_obj_set_pos(label(scr, &lv_font_montserrat_12, col::CHEVRON, 1, "v" FIRMWARE_VERSION), 16, FOOT_Y);

    paintArcs(0);
    lastTickMs = millis();
    timer = lv_timer_create(tick, TICK_MS, nullptr);
    lv_screen_load(scr);
}

} // namespace

bool splash::active() { return up && phase != FINISHED; }
void splash::onFinished(void (*load)()) { loadMain = load; }

void display::splash()
{
    lv_display_t *disp = board::begin(firstFrame);
    if (!disp)
    {
        ESP_LOGE(TAG, "no display");
        return;
    }
    // board::begin() returns holding the LVGL lock: nothing is drawn till the splash is in.
    lv_theme_t *theme = lv_theme_default_init(disp, lv_color_hex(col::ACCENT), lv_color_hex(col::OK), true,
                                              &lv_font_montserrat_16);
    lv_display_set_theme(disp, theme);
    build();
    up = true;
    board::unlock();
    bootlog::mark("splash");
}

void display::bootStep(const char *label)
{
    if (!up) return;
    stepLabel = label;
    stepIndex = stepIndex + 1;
}

void display::bootDone()
{
    if (up) workDone = true;
}

#endif // LAPDOG_TOUCH
