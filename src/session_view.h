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
};

void frame(const lapframe::Frame &f);
State get();

} // namespace session_view
