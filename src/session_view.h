#pragma once
// What the session is doing, kept up from the same frames the app gets, for the
// small status screens (the OLED, the 1.47" LCD): heat, the lap in progress and its
// start, the last and best lap. frame() is called from the timing loop, get() from
// the screen's own task.

#include <stdint.h>

#include "lap_frame.h"

namespace session_view
{

struct State
{
    bool armed;
    bool everArmed;
    uint8_t mode;
    uint8_t stage;
    uint16_t heat;
    uint8_t lapLimit;
    uint8_t laps;      // completed
    bool started;      // the start pass (practice) or holeshot (race) is in
    uint32_t lapAtMs;  // local clock at the last pass: the lap in progress started then
    uint32_t lastMs;   // last lap's time
    bool lastBest;
    uint32_t bestMs;
    uint32_t raceMs;   // total at the finish
    uint32_t flashUntilMs;
    // For the 1.47" screen: the lap just finished is held on screen for a moment, with how
    // it compared to the best before it.
    uint8_t lastLap;       // number of the last lap
    uint8_t bestLap;
    bool hasDelta;         // there was a best before the last lap
    int32_t deltaMs;       // last lap minus the best before it (negative: a new best)
    uint32_t holdUntilMs;  // local clock: show the last lap's time until then
    uint32_t sumMs;        // of the completed laps, for the average
    // The last finished heat, kept across the next arm: the idle screen's summary.
    bool haveLast;
    uint16_t lastHeat;
    uint8_t lastLaps;
    uint32_t lastBestMs, lastAvgMs;
};

void frame(const lapframe::Frame &f);
State get();

} // namespace session_view
