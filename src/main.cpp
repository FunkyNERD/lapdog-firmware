// LapDog - sits by the gate, barks on every lap.
//
// Idle mode: radio free, config over USB. Timer mode: radio dedicated to SDR
// capture, laps and race messages out over USB plus BLE *or* ESP-NOW.
// Long-press BOOT to toggle; short press starts the next heat.

#include <stdio.h>

#include "app.h"
#include "button.h"
#include "config.h"
#include "console.h"
#include "detector.h"
#include "display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led.h"
#include "outputs.h"
#include "race.h"
#include "sdr.h"
#include "settings.h"
#include "vrxc_msp.h"

static const char *TAG = "lapdog";

enum CmdType
{
    CMD_ARM,
    CMD_DISARM,
    CMD_NEW_HEAT,
    CMD_RESTART,
};

struct Cmd
{
    CmdType type;
    app::ArmRequest arm;
};

static QueueHandle_t cmds;
static volatile bool simRequested = false;
static volatile bool rssiStream = false;
static volatile race::Stage currentStage = race::IDLE;
static volatile uint16_t currentHeat = 0;
static uint32_t idleBeatsUntil = 0; // millis(): idle heartbeats until then, after a disarm (0 = none)

// The session's messages go to the radios and USB, and to the OLED if there is one.
class TeeSink : public race::Sink
{
public:
    void frame(const lapframe::Frame &f) override
    {
        display::frame(f);
        outputs::sink().frame(f);
    }
    void osd(uint8_t row, const char *text) override { outputs::sink().osd(row, text); }
    void osdClear() override { outputs::sink().osdClear(); }
    void flush() override { outputs::sink().flush(); }
};

static TeeSink tee;
static race::Session session(tee);
static app::ArmRequest lastArm = {};

bool app::armed() { return currentStage != race::IDLE; }
race::Stage app::stage() { return currentStage; }
uint16_t app::sessionHeat() { return currentHeat; }
void app::simulatePass() { simRequested = true; }
void app::setRssiStream(bool on) { rssiStream = on; }

static void send(CmdType t, const app::ArmRequest *r = nullptr)
{
    Cmd c = {t, r ? *r : app::ArmRequest{}};
    xQueueSend(cmds, &c, 0);
}
void app::requestArm(const ArmRequest &r) { send(CMD_ARM, &r); }
void app::requestDisarm() { send(CMD_DISARM); }
void app::requestNewHeat() { send(CMD_NEW_HEAT); }
void app::requestRestart() { send(CMD_RESTART); }

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

static void disarm()
{
    session.disarm(millis());
    sdr::end();
    idleBeatsUntil = millis() + cfg::IDLE_HEARTBEATS_FOR_MS;
    if (!idleBeatsUntil) idleBeatsUntil = 1;
    currentStage = race::IDLE;
    led::set(led::IDLE);
    ESP_LOGI(TAG, "idle");
}

// heat: the heat number to run (a new one comes from settings::nextHeat()).
static void arm(const app::ArmRequest &r, uint16_t heat, Detector &det)
{
    Settings &s = settings::get();
    // Links and bind phrase may have changed while idle.
    uint8_t uid[6];
    vrxc::uidFromBindPhrase(s.bindPhrase, uid);
    outputs::setLinks(s.links, uid);

    race::Setup setup = timer_settings::sessionSetup(s, r.mode, heat);
    if (r.overrideLimit)
    {
        setup.limit = r.limit;
        if (r.limit == race::LIMIT_LAPS) setup.laps = r.laps;
        else setup.timeS = r.timeS;
    }
    lastArm = r;
    det = Detector(timer_settings::detectorParams(s));
    // TODO: measure the in-room baseline over a second or so instead.
    det.setBaseline(-90.0f);
    idleBeatsUntil = 0;
    sdr::begin(s.freqMhz);
    session.arm(setup, timer_settings::holdMs(s, esp_random()));
    currentHeat = heat;
    currentStage = session.stage();
    led::set(led::ARMED);
    ESP_LOGI(TAG, "armed: heat %u, %s", heat, r.mode == race::RACE ? "race" : "practice");
}

static void armNewHeat(const app::ArmRequest &r, Detector &det)
{
    uint16_t heat = settings::nextHeat();
    settings::setNextHeat((uint16_t)(heat + 1));
    arm(r, heat, det);
}

static void handle(const Cmd &c, Detector &det)
{
    switch (c.type)
    {
    case CMD_ARM:
        if (!app::armed()) armNewHeat(c.arm, det);
        break;
    case CMD_DISARM:
        if (app::armed()) disarm();
        break;
    case CMD_NEW_HEAT:
        if (!app::armed()) break;
        session.disarm(millis()); // finishes this heat, so LapDog LCD logs it
        armNewHeat(lastArm, det);
        break;
    case CMD_RESTART:
        if (!app::armed()) break;
        session.disarm(millis(), false); // same heat again: abandon this run
        arm(lastArm, currentHeat, det);
        break;
    }
}

static app::ArmRequest buttonArm()
{
    app::ArmRequest r = {};
    r.mode = (race::Mode)settings::get().mode;
    return r;
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "%s", "LapDog timer v" FIRMWARE_VERSION);
    cmds = xQueueCreate(4, sizeof(Cmd));
    settings::begin();
    led::begin();
    display::begin();
    button::begin();
    console::begin();

    uint8_t uid[6];
    vrxc::uidFromBindPhrase(settings::get().bindPhrase, uid);
    outputs::setLinks(settings::get().links, uid);
    ESP_LOGI(TAG, "UID %02x:%02x:%02x:%02x:%02x:%02x", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);

    Detector det(timer_settings::detectorParams(settings::get()));
    led::set(led::IDLE);
    if (cfg::BOOT_ARMED) armNewHeat(buttonArm(), det);
    uint32_t now = millis();
    uint32_t lastHeartbeat = now;
    uint32_t lastRssi = now;
    bool calibrating = false; // SDR running in idle for "rssi on"

    while (true)
    {
        now = millis();

        Cmd c;
        while (xQueueReceive(cmds, &c, 0) == pdTRUE) handle(c, det);

        switch (button::poll(now))
        {
        case button::LONG_PRESS:
            if (app::armed()) disarm();
            else armNewHeat(buttonArm(), det);
            break;
        case button::SHORT_PRESS:
            if (app::armed()) handle({CMD_NEW_HEAT, {}}, det);
            break;
        case button::NONE: break;
        }

        // Live signal for calibration works idle too: the radio is free then.
        bool wantIdleSdr = !app::armed() && rssiStream;
        if (wantIdleSdr != calibrating)
        {
            calibrating = wantIdleSdr;
            if (calibrating)
            {
                det = Detector(timer_settings::detectorParams(settings::get()));
                det.setBaseline(-90.0f);
                sdr::begin(settings::get().freqMhz);
            }
            else if (!app::armed())
                sdr::end();
        }

        if (simRequested)
        {
            simRequested = false;
            sdr::simulatePass(now);
        }

        if (app::armed() || calibrating)
        {
            Pass p;
            PassResult r = det.feed(sdr::bandPowerDb(now), now, p);
            if (r == PassResult::PASS && app::armed())
            {
                led::flash(now);
                session.pass(p.atMs); // the session times laps itself (holeshot runs from GO)
                ESP_LOGI(TAG, "pass at %lu", (unsigned long)p.atMs);
            }
            else if ((r == PassResult::TOO_SOON || r == PassResult::TOO_LONG) && app::armed())
            {
                bool soon = r == PassResult::TOO_SOON;
                session.ignored(soon ? lapframe::IGNORED_TOO_SOON : lapframe::IGNORED_TOO_LONG,
                                soon ? p.lapMs : p.durationMs);
                ESP_LOGI(TAG, "pass ignored: %s", soon ? "too soon" : "too long (parked?)");
            }
            if (rssiStream && now - lastRssi >= 50)
            {
                lastRssi = now;
                printf("R:%.1f\n", det.smoothed() - det.baseline());
            }
        }

        // Also while idle: FINISHED and the last lap result still clear after disarm.
        race::Stage before = session.stage();
        session.tick(now);
        if (app::armed())
        {
            // Laps (and the minimum lap time) count from GO, not from passes while staging.
            if (before == race::STAGING && session.stage() == race::RACING) det.reset();
            if (now - lastHeartbeat >= cfg::HEARTBEAT_MS)
            {
                lastHeartbeat = now;
                session.heartbeat(now);
            }
            currentStage = session.stage();
        }
        else if (idleBeatsUntil)
        {
            if ((int32_t)(idleBeatsUntil - now) <= 0) idleBeatsUntil = 0;
            else if (now - lastHeartbeat >= cfg::IDLE_HEARTBEAT_MS)
            {
                lastHeartbeat = now;
                session.heartbeat(now); // stage IDLE: the phone ends the heat if it missed DISARMED
            }
        }

        led::update(now);
        vTaskDelay(pdMS_TO_TICKS(cfg::SAMPLE_PERIOD_MS));
    }
}
