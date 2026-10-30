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
        st = {};
        st.armed = st.everArmed = true;
        st.mode = f.mode;
        st.heat = f.heat;
        st.lapLimit = f.lapLimit;
        st.stage = f.mode == race::RACE ? race::STAGING : race::PRACTICE_RUN;
        break;
    case lapframe::DISARMED:
        st.armed = false;
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
            st.laps = f.lap;
            st.lastMs = f.ms;
            st.lastBest = f.flags & lapframe::LAP_BEST;
            if (st.lastBest || !st.bestMs) st.bestMs = f.ms;
            if (f.flags & lapframe::LAP_FINAL) st.stage = race::FINISHED;
        }
        st.flashUntilMs = now + 300;
        break;
    default: break;
    }
    portEXIT_CRITICAL(&lock);
}
