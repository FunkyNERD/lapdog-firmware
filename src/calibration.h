#pragma once
// Receiver calibration on the timer (issue #12): the device side of
// include/calibrate.h. Driven from the USB console ("cal ..."), run by the
// timing loop, which owns the radio. Idle only.
//
//   cal floor   quad off: measure the floor at every gain (~15 s)
//   cal fly [g] fly laps through the gate at gain g (default: the current
//               gain), then "cal stop" (or it stops by itself after 2 min);
//               prints the advice: lower/raise the gain and fly again, more
//               laps, or ok with enter/exit
//   cal auto [g] hands-free: rounds of "cal fly" that end after 6 passes (or
//               90 s), re-fly at the advised gain (3 rounds at most), and apply
//               the first ok result. Progress: S:cal.round=<n> <gain> as each
//               round starts, cal.live_passes as each pass is found, cal.fly_s;
//               then S:cal.auto=ok|failed and S:cal.done=auto.
//   cal apply   keep the last ok advice: gain, floor and thresholds saved
//   cal clear   back to the defaults: gain, floor (cfg::DEFAULT_FLOOR_DB, or a
//               measured one) and thresholds
//   cal         status
//
// Saved in NVS under its own keys, not the Settings blob: "cal" (gain, floor,
// channel) and "calfloor" (the floor at every gain, for the channel measured).
// The detector's baseline is always a fixed floor, with calib::MERGE_MS merging:
// the calibrated (or measured) floor at the gain in use, else cfg::DEFAULT_FLOOR_DB.
// Version 1 records (before the presets' reference) are converted on load.

#include <stdint.h>

#include "detector.h"
#include "sdr.h"

namespace calibration
{

void begin(); // after settings::begin(): load, and set the saved gain

bool valid();
int gain();
// The presets' reference (presets.h): what calibration set, else the defaults.
float refEnterDb();
float refExitDb();
float floorDb(); // at the gain in use (from the floor table when it has one)

// Sets up a detector for a session: fixed floor and merging when calibrated,
// tracked baseline otherwise.
void configure(Detector &d);

// Console requests (any task). false: refused (armed, busy, no floor yet...).
bool requestFloor();
bool requestFly(int gain); // -1 = the current gain
bool requestAuto(int gain); // -1 = the calibrated gain, or cfg::SDR_GAIN
bool requestStop();
bool requestApply();
bool requestClear();
void printStatus();
void printSummary(); // the few lines "get" adds

// What the screens show while calibrating, and for a while after.
struct View
{
    enum Step : uint8_t { NONE, FLOOR, FLY, RESULT };
    uint8_t step;
    uint8_t at, of;  // floor: gains done of all
    uint8_t round;   // fly: round of "cal auto" (0 = a single "cal fly")
    uint8_t passes;  // fly: passes found so far
    int8_t gain;
    uint16_t seconds;
    uint8_t result;  // RESULT: a calib::Verdict
    uint32_t until;  // RESULT: shown until (ms)
};
View view();

// Timing loop.
bool active(); // wants the radio
void sample(uint32_t nowMs, float db, const sdr::Snapshot &s);
void tick(uint32_t nowMs, bool armed);

} // namespace calibration
