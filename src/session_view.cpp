#include "session_view.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "race.h"

static session_view::State st = {};
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000); }

session_view::State session_view::get()
{
    portENTER_CRITICAL(&lock);
    State s = st;
    portEXIT_CRITICAL(&lock);
    return s;
}

void session_view::frame(const lapframe::Frame &f)
{
    uint32_t now = millis();
    portENTER_CRITICAL(&lock);
    switch (f.type)
    {
    case lapframe::ARMED:
    {
        State keep = st; // the last heat's summary outlives the reset
        st = {};
        st.haveLast = keep.haveLast;
        st.lastHeat = keep.lastHeat;
        st.lastLaps = keep.lastLaps;
        st.lastBestMs = keep.lastBestMs;
        st.lastAvgMs = keep.lastAvgMs;
        st.armed = st.everArmed = true;
        st.mode = f.mode;
        st.heat = f.heat;
        st.lapLimit = f.lapLimit;
        st.stage = f.mode == race::RACE ? race::STAGING : race::PRACTICE_RUN;
        break;
    }
    case lapframe::DISARMED:
        st.armed = false;
        if (st.laps)
        {
            st.haveLast = true;
            st.lastHeat = st.heat;
            st.lastLaps = st.laps;
            st.lastBestMs = st.bestMs;
            st.lastAvgMs = st.sumMs / st.laps;
        }
        break;
    case lapframe::HEARTBEAT:
        st.stage = f.stage;
        break;
    case lapframe::RACE:
        st.stage = f.stage;
        if (f.stage == race::FINISHED) st.raceMs = f.ms;
        break;
    case lapframe::LAP:
        st.started = true;
        st.lapAtMs = now;
        if (f.lap > 0)
        {
            st.hasDelta = st.bestMs != 0;
            st.deltaMs = (int32_t)f.ms - (int32_t)st.bestMs;
            st.laps = st.lastLap = f.lap;
            st.sumMs += f.ms;
            st.holdUntilMs = now + 2500;
            st.lastMs = f.ms;
            st.lastBest = f.flags & lapframe::LAP_BEST;
            if (st.lastBest || !st.bestMs)
            {
                st.bestMs = f.ms;
                st.bestLap = f.lap;
            }
            if (f.flags & lapframe::LAP_FINAL) st.stage = race::FINISHED;
        }
        st.flashUntilMs = now + 300;
        break;
    default: break;
    }
    portEXIT_CRITICAL(&lock);
}
