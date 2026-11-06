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
//   cal apply   keep the last ok advice: gain, floor and thresholds saved
//   cal clear   back to the tracked baseline (thresholds are left as they are)
//   cal         status
//
// Saved in NVS under its own keys, not the Settings blob: "cal" (gain, floor,
// channel) and "calfloor" (the floor at every gain, for the channel measured).
// When calibrated, the detector's baseline is the floor at the gain in use,
// fixed, with calib::MERGE_MS merging; otherwise it tracks, as before.

#include <stdint.h>

#include "detector.h"
#include "sdr.h"

namespace calibration
{

void begin(); // after settings::begin(): load, and set the saved gain

bool valid();
int gain();
float floorDb(); // at the gain in use (from the floor table when it has one)

// Sets up a detector for a session: fixed floor and merging when calibrated,
// tracked baseline otherwise.
void configure(Detector &d);

// Console requests (any task). false: refused (armed, busy, no floor yet...).
bool requestFloor();
bool requestFly(int gain); // -1 = the current gain
bool requestStop();
bool requestApply();
bool requestClear();
void printStatus();

// Timing loop.
bool active(); // wants the radio
void sample(uint32_t nowMs, float db, const sdr::Snapshot &s);
void tick(uint32_t nowMs, bool armed);

} // namespace calibration
