#pragma once
// Gate sensitivity presets (issue #12): steps around a reference, not fixed numbers.
// The reference is the calibrated enter/exit on a calibrated timer, else the defaults
// (cfg::DEFAULT_ENTER_DB / DEFAULT_EXIT_DB). Both are dB above the receiver floor.
//   Low    +3 dB: fewer false reads (a section of track near the timer)
//   Medium the reference
//   High   -3 dB: catches weaker passes (missed laps, weak VTX, a far gate)
// Pure logic, unit-tested natively (test/test_calibrate). The app mirrors it.

namespace presets
{

enum Kind
{
    LOW,
    MEDIUM,
    HIGH,
    CUSTOM,
};

constexpr float STEP_DB = 3.0f;

inline float offset(Kind k) { return k == LOW ? STEP_DB : k == HIGH ? -STEP_DB : 0.0f; }

// The thresholds a preset sets, from the reference.
inline void apply(Kind k, float refEnter, float refExit, float &enter, float &exit)
{
    float o = offset(k);
    enter = refEnter + o;
    exit = refExit + o < 0 ? 0 : refExit + o;
}

// Which preset these thresholds are (to 0.05 dB), or CUSTOM.
inline Kind of(float enter, float exit, float refEnter, float refExit)
{
    for (int i = LOW; i <= HIGH; i++)
    {
        Kind k = (Kind)i;
        float e, x;
        apply(k, refEnter, refExit, e, x);
        float de = e - enter, dx = x - exit;
        if (de < 0.05f && de > -0.05f && dx < 0.05f && dx > -0.05f) return k;
    }
    return CUSTOM;
}

inline const char *name(Kind k) { return k == LOW ? "Low" : k == MEDIUM ? "Medium" : k == HIGH ? "High" : "Custom"; }

} // namespace presets
