#include "calibration.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "calibrate.h"
#include "cloud.h"
#include "config.h"
#include "esp_log.h"
#include "led.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "cal";
static const char *NVS_NS = "lapdog";

// --- Saved state ----------------------------------------------------------------

static constexpr uint8_t CAL_VERSION = 1;
static constexpr unsigned MAX_GAINS = 128;

struct CalRecord
{
    uint8_t version;
    uint8_t valid;
    int16_t gain;
    uint16_t freqMhz; // the channel it was calibrated on
    float floorDb;    // at gain, quad off
};

struct FloorTable
{
    uint16_t freqMhz; // 0 = none
    uint16_t count;   // gains 0..count-1
    float db[MAX_GAINS];
};

static CalRecord cal = {};
// Heap, not static: on the touch build static data only just fits below the
// SDR's capture bank (esp_sdr/sram_guard.ld).
static FloorTable *floorsP = nullptr;
#define floors (*floorsP)

static bool load(const char *key, void *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t got = len;
    bool ok = nvs_get_blob(h, key, out, &got) == ESP_OK && got == len;
    nvs_close(h);
    return ok;
}

static bool store(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, key, data, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) ESP_LOGW(TAG, "couldn't save %s", key);
    return ok;
}

static bool haveFloors() { return floorsP && floors.count && floors.freqMhz == settings::get().freqMhz; }

// --- Run state (timing loop) -------------------------------------------------------

enum class State : uint8_t
{
    IDLE,
    FLOOR,
    FLY,
};

enum class Req : uint8_t
{
    NONE,
    FLOOR,
    FLY,
    AUTO,
    STOP,
    APPLY,
    CLEAR,
};

static volatile State state = State::IDLE;
static volatile Req req = Req::NONE;
static volatile int reqGain = -1;

// Floor sweep
static constexpr unsigned FLOOR_SETTLE = 3;   // captures dropped after a gain change
static constexpr unsigned FLOOR_SAMPLES = 16; // averaged per gain
static int sweepGain = 0, sweepMax = 0, savedGain = -1;
static unsigned sweepSkip = 0, sweepCount = 0;
static float sweepSum = 0;
static FloorTable *sweepP = nullptr;
#define sweep (*sweepP)
static bool sweepDone = false;

// Flight
static constexpr unsigned FLY_BUCKETS = 1200; // 2 minutes at calib::BUCKET_MS
static constexpr uint32_t FLY_MAX_MS = FLY_BUCKETS * calib::BUCKET_MS;
static constexpr unsigned FLY_SETTLE = 5;
static calib::Bucket *buckets = nullptr;
static calib::Recorder *rec = nullptr;
static int flyGain = 0;
static uint32_t flyStartMs = 0;
static unsigned flySkip = 0;
static bool flyDone = false;
static int lowestClipped = -1; // lowest gain flown this boot that clipped
static calib::PassCounter passCounter;
static unsigned livePasses = 0;

// "cal auto": rounds of flying until a result is ok, then apply.
static bool autoMode = false;
static unsigned autoRound = 0;

// For the screens and the LED (read from other tasks; a torn read just shows late).
static calibration::View shown = {};
static uint32_t lastProgressMs = 0;
static bool ledShowing = false;

// The last flight's result
static bool haveAdvice = false;
static calib::Flight lastFlight;
static calib::Advice lastAdvice;
static int lastGain = 0;

// --- Queries -----------------------------------------------------------------------

void calibration::begin()
{
    CalRecord r;
    if (load("cal", &r, sizeof(r)) && r.version == CAL_VERSION) cal = r;
    floorsP = (FloorTable *)calloc(1, sizeof(FloorTable));
    if (floorsP && (!load("calfloor", floorsP, sizeof(FloorTable)) || floors.count > MAX_GAINS)) *floorsP = {};
    if (cal.valid)
    {
        sdr::setGain(cal.gain);
        ESP_LOGI(TAG, "calibrated: gain %d, floor %.1f dBFS at %u MHz", cal.gain, cal.floorDb, cal.freqMhz);
    }
}

bool calibration::valid() { return cal.valid; }
int calibration::gain() { return cal.gain; }

float calibration::floorDb()
{
    int g = sdr::gain();
    if (haveFloors() && g >= 0 && g < floors.count) return floors.db[g];
    return cal.floorDb;
}

void calibration::configure(Detector &d)
{
    if (cal.valid)
    {
        // A session times at the calibrated gain, whatever a calibration round (or
        // "sdr gain") left the radio at: the thresholds were measured there.
        if (state == State::IDLE && sdr::gain() != cal.gain) sdr::setGain(cal.gain);
        d.setBaseline(floorDb());
        d.mergeWithin(calib::MERGE_MS);
    }
    else d.trackBaseline(cfg::BASELINE_RISE);
}

bool calibration::active() { return state != State::IDLE; }

// --- Requests (console task) -----------------------------------------------------------

static bool request(Req r)
{
    if (req != Req::NONE || app::armed() || cloud::busy()) return false;
    req = r;
    return true;
}

bool calibration::requestFloor() { return state == State::IDLE && sdr::available() && request(Req::FLOOR); }

static bool requestFlight(int g, Req kind)
{
    if (state != State::IDLE || !sdr::available() || !haveFloors()) return false;
    if (g > (int)floors.count - 1) return false;
    reqGain = g;
    return request(kind);
}

bool calibration::requestFly(int g) { return requestFlight(g, Req::FLY); }

bool calibration::requestAuto(int g)
{
    return requestFlight(g < 0 ? (cal.valid ? cal.gain : calib::AUTO_START_GAIN) : g, Req::AUTO);
}

bool calibration::requestStop() { return state == State::FLY && request(Req::STOP); }
bool calibration::requestApply()
{
    return state == State::IDLE && haveAdvice && lastAdvice.verdict == calib::Verdict::OK && request(Req::APPLY);
}
bool calibration::requestClear() { return state == State::IDLE && request(Req::CLEAR); }

// --- Printing ----------------------------------------------------------------------------

static const char *verdictName(calib::Verdict v)
{
    switch (v)
    {
    case calib::Verdict::OK: return "ok";
    case calib::Verdict::LOWER_GAIN: return "lower";
    case calib::Verdict::RAISE_GAIN: return "raise";
    case calib::Verdict::TOO_FEW_PASSES: return "more";
    }
    return "?";
}

calibration::View calibration::view() { return shown; }

// For "get": enough for an app to know where the timer stands.
void calibration::printSummary()
{
    printf("S:cal=%s\n", cal.valid ? "on" : "off");
    if (cal.valid) printf("S:cal.gain=%d\nS:cal.floor=%.1f\n", cal.gain, floorDb());
    printf("S:cal.state=%s\n", state == State::FLOOR ? "floor" : state == State::FLY ? (autoMode ? "auto" : "fly") : "idle");
    printf("S:cal.floors=%s\n", haveFloors() ? "yes" : "no");
}

static void printAdvice()
{
    const calib::Flight &f = lastFlight;
    const calib::Advice &a = lastAdvice;
    printf("S:cal.result=%s\n", verdictName(a.verdict));
    printf("S:cal.flown_gain=%d\nS:cal.next_gain=%d\n", lastGain, a.gain);
    printf("S:cal.passes=%u\nS:cal.clipped=%u\n", f.passes, f.clippedPasses);
    printf("S:cal.inflight=%.1f\nS:cal.pass=%.1f\n", f.inFlightDb, f.passDb);
    printf("S:cal.contrast=%.1f\nS:cal.headroom=%.1f\nS:cal.low_contrast=%d\n", a.contrastDb, a.headroomDb,
           a.lowContrast ? 1 : 0);
    printf("S:cal.enter=%.1f\nS:cal.exit=%.1f\n", a.enterDb, a.exitDb);
}

void calibration::printStatus()
{
    const Settings &s = settings::get();
    printf("S:cal=%s\n", cal.valid ? "on" : "off");
    const char *st = state == State::FLOOR ? "floor" : state == State::FLY ? (autoMode ? "auto" : "fly") : "idle";
    printf("S:cal.state=%s\n", st);
    if (cal.valid)
    {
        printf("S:cal.gain=%d\nS:cal.floor=%.1f\nS:cal.freq=%u\n", cal.gain, floorDb(), cal.freqMhz);
        if (cal.freqMhz != s.freqMhz) printf("S:cal.warn=calibrated on %u MHz, now on %u\n", cal.freqMhz, s.freqMhz);
        if (sdr::gain() != cal.gain) printf("S:cal.warn=gain %d in use, calibrated at %d\n", sdr::gain(), cal.gain);
        if (s.detector.enterDb < 12) printf("S:cal.warn=enter %.1f looks like a preset, not calibrated\n", s.detector.enterDb);
    }
    if (floorsP && floors.count) printf("S:cal.floors=%u gains at %u MHz%s\n", floors.count, floors.freqMhz, haveFloors() ? "" : " (other channel)");
    else printf("S:cal.floors=none\n");
    if (state == State::FLY && rec) printf("S:cal.fly_s=%.0f\n", rec->size() * calib::BUCKET_MS / 1000.0);
    if (haveAdvice) printAdvice();
}

// --- Timing loop ---------------------------------------------------------------------------

static void startFloor()
{
    savedGain = sdr::gain();
    sweepMax = 0; // known once the radio has tuned (first sample)
    sweepGain = 0;
    sweepSkip = FLOOR_SETTLE;
    sweepCount = 0;
    sweepSum = 0;
    sweepDone = false;
    if (!sweepP) sweepP = (FloorTable *)malloc(sizeof(FloorTable));
    if (!sweepP || !floorsP)
    {
        printf("S:cal.error=no memory\n");
        return;
    }
    sweep = {};
    sdr::setGain(0);
    state = State::FLOOR;
    shown = {};
    shown.step = calibration::View::FLOOR;
    printf("S:cal.started=floor\n");
}

static void startFly(int g)
{
    if (!buckets) buckets = (calib::Bucket *)malloc(FLY_BUCKETS * sizeof(calib::Bucket));
    if (!rec && buckets) rec = new calib::Recorder(buckets, FLY_BUCKETS);
    if (!rec)
    {
        printf("S:cal.error=no memory\n");
        return;
    }
    rec->reset();
    passCounter.reset();
    livePasses = 0;
    flyGain = g < 0 ? sdr::gain() : g;
    if (flyGain < 0) flyGain = cfg::SDR_GAIN; // the AGC: fly at the default instead
    sdr::setGain(flyGain);
    flySkip = FLY_SETTLE;
    flyStartMs = 0;
    flyDone = false;
    state = State::FLY;
    shown.step = calibration::View::FLY;
    shown.round = (uint8_t)(autoMode ? autoRound : 0);
    shown.gain = (int8_t)flyGain;
    shown.passes = 0;
    shown.seconds = 0;
    // One line per round, so an app knows which round at which gain (0 = a single "cal fly").
    printf("S:cal.started=fly\nS:cal.round=%u %d\n", autoMode ? autoRound : 0, flyGain);
}

static void freeFly()
{
    delete rec;
    rec = nullptr;
    free(buckets);
    buckets = nullptr;
}

static void doApply()
{
    Settings &s = settings::get();
    cal = {CAL_VERSION, 1, (int16_t)lastGain, s.freqMhz, floors.db[lastGain]};
    store("cal", &cal, sizeof(cal));
    s.detector.enterDb = roundf(lastAdvice.enterDb * 10) / 10;
    s.detector.exitDb = roundf(lastAdvice.exitDb * 10) / 10;
    settings::save();
    sdr::setGain(lastGain);
    printf("S:cal=on\nS:cal.gain=%d\nS:cal.floor=%.1f\nS:enter=%.1f\nS:exit=%.1f\nS:cal.done=apply\n", cal.gain,
           cal.floorDb, s.detector.enterDb, s.detector.exitDb);
}

static void showResult(uint32_t nowMs, uint8_t result)
{
    shown.step = calibration::View::RESULT;
    shown.result = result;
    shown.gain = (int8_t)lastGain;
    shown.until = nowMs + 20000;
}

static void finishFly(uint32_t nowMs)
{
    lastFlight = calib::analyse(*rec);
    lastGain = flyGain;
    float floor = floors.db[flyGain];
    if (lastFlight.clippedPasses && (lowestClipped < 0 || flyGain < lowestClipped)) lowestClipped = flyGain;
    lastAdvice = calib::advise(lastFlight, floor, flyGain, floors.count - 1, lowestClipped);
    haveAdvice = true;
    state = State::IDLE;
    // The next "cal fly" defaults to the suggested gain.
    if (lastAdvice.gain != flyGain) sdr::setGain(lastAdvice.gain);
    printAdvice();
    printf("S:cal.done=fly\n");
    if (!autoMode)
    {
        freeFly();
        showResult(nowMs, (uint8_t)lastAdvice.verdict);
        return;
    }
    if (lastAdvice.verdict == calib::Verdict::OK)
    {
        autoMode = false;
        freeFly();
        doApply();
        showResult(nowMs, (uint8_t)calib::Verdict::OK);
        printf("S:cal.auto=ok\nS:cal.done=auto\n");
        return;
    }
    if (autoRound < calib::AUTO_ROUNDS)
    {
        autoRound++;
        startFly(lastAdvice.gain); // keeps the buffers
        return;
    }
    autoMode = false;
    freeFly();
    showResult(nowMs, (uint8_t)lastAdvice.verdict);
    printf("S:cal.auto=failed\nS:cal.done=auto\n");
}

void calibration::sample(uint32_t nowMs, float db, const sdr::Snapshot &s)
{
    if (state == State::FLOOR && !sweepDone)
    {
        if (!sweepMax)
        {
            sweepMax = (int)sdr::gainMax();
            if (sweepMax >= (int)MAX_GAINS) sweepMax = MAX_GAINS - 1;
            if (!sweepMax) return;
        }
        if (sweepSkip)
        {
            sweepSkip--;
            return;
        }
        sweepSum += db;
        if (++sweepCount < FLOOR_SAMPLES) return;
        sweep.db[sweepGain] = sweepSum / sweepCount;
        if (++sweepGain > sweepMax)
        {
            sweep.count = (uint16_t)(sweepMax + 1);
            sweep.freqMhz = settings::get().freqMhz;
            sweepDone = true; // saved from tick()
            return;
        }
        sdr::setGain(sweepGain);
        sweepSkip = FLOOR_SETTLE;
        sweepCount = 0;
        sweepSum = 0;
    }
    else if (state == State::FLY && !flyDone)
    {
        if (flySkip)
        {
            flySkip--;
            return;
        }
        if (!flyStartMs) flyStartMs = nowMs ? nowMs : 1;
        rec->add(nowMs, db, s.clipped > 0);
        if (unsigned n = passCounter.update(*rec))
        {
            livePasses += n;
            shown.passes = (uint8_t)(livePasses > 255 ? 255 : livePasses);
            printf("S:cal.live_passes=%u\n", livePasses);
        }
        uint32_t limit = autoMode ? calib::AUTO_ROUND_MS : FLY_MAX_MS;
        if (nowMs - flyStartMs >= limit || rec->size() >= FLY_BUCKETS) flyDone = true;
        if (autoMode && livePasses >= calib::AUTO_PASSES) flyDone = true;
    }
}

void calibration::tick(uint32_t nowMs, bool armed)
{
    if (armed && state != State::IDLE)
    {
        autoMode = false;
        shown = {};
        // Armed from the button mid-calibration: the session wins.
        if (state == State::FLOOR)
        {
            sdr::setGain(savedGain);
            free(sweepP);
            sweepP = nullptr;
        }
        if (state == State::FLY) freeFly();
        state = State::IDLE;
        printf("S:cal.done=aborted\n");
    }

    if (state == State::FLOOR && sweepDone)
    {
        floors = sweep;
        free(sweepP);
        sweepP = nullptr;
        store("calfloor", floorsP, sizeof(FloorTable));
        sdr::setGain(savedGain);
        state = State::IDLE;
        printf("S:cal.floors=%u gains at %u MHz\n", floors.count, floors.freqMhz);
        int g = savedGain >= 0 && savedGain < floors.count ? savedGain : cfg::SDR_GAIN;
        if (g < floors.count) printf("S:cal.floor=%.1f\nS:cal.floor_gain=%d\n", floors.db[g], g);
        printf("S:cal.done=floor\n");
    }
    if (state == State::FLY && flyDone) finishFly(nowMs);

    // Progress, about once a second, for an app to show (and say).
    if (state != State::IDLE && nowMs - lastProgressMs >= 1000)
    {
        lastProgressMs = nowMs;
        if (state == State::FLOOR && sweepMax)
        {
            shown.at = (uint8_t)sweepGain;
            shown.of = (uint8_t)(sweepMax + 1);
            printf("S:cal.floor_at=%d/%d\n", sweepGain, sweepMax + 1);
        }
        else if (state == State::FLY && flyStartMs)
        {
            shown.seconds = (uint16_t)((nowMs - flyStartMs) / 1000);
            printf("S:cal.fly_s=%u\n", shown.seconds);
        }
    }
    if (shown.step == calibration::View::RESULT && (int32_t)(nowMs - shown.until) > 0) shown = {};
    if (shown.step == calibration::View::FLOOR && state == State::IDLE) shown = {};

    // The LED: its own pattern while calibrating; the session's otherwise.
    bool showLed = state != State::IDLE && !armed;
    if (showLed != ledShowing)
    {
        ledShowing = showLed;
        if (!armed) led::set(showLed ? led::CALIBRATING : led::IDLE);
    }

    Req r = req;
    if (r == Req::NONE) return;
    req = Req::NONE;
    switch (r)
    {
    case Req::FLOOR: startFloor(); break;
    case Req::FLY:
        autoMode = false;
        startFly(reqGain);
        break;
    case Req::AUTO:
        autoMode = true;
        autoRound = 1;
        startFly(reqGain);
        break;
    case Req::STOP:
        if (state == State::FLY)
        {
            flyDone = true; // finished on the next tick
            autoMode = false; // and no more rounds
        }
        break;
    case Req::APPLY:
        doApply();
        break;
    case Req::CLEAR:
        cal.valid = 0;
        store("cal", &cal, sizeof(cal));
        sdr::setGain(cfg::SDR_GAIN);
        printf("S:cal=off\nS:cal.done=clear\n");
        break;
    case Req::NONE: break;
    }
}
