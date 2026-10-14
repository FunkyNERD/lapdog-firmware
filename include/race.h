#pragma once
// One timing session (a heat), practice or race, and the race messages it
// sends. Pure logic, no IDF - unit-tested natively (test/test_race).
//
//   Practice: arm -> first gate pass starts lap 1 -> laps until disarmed.
//   Race:     arm -> STAGING ("ARM NOW") -> hold (random 1-3 s) -> GO ->
//             first pass is the holeshot -> laps -> FINISHED when the lap
//             limit is reached, or on the first pass after the time limit.
// Both modes: passes the detector rejected (too soon / parked quad) are
// reported with ignored() and counted per heat, never silently dropped.
//
// Everything the session says goes to a Sink as LapDog frames (USB + BLE)
// and as VRxC_ELRS OSD rows (ESP-NOW to goggles/RacePanel). OSD rows are also
// sent as TEXT frames, so the app shows exactly what the goggles show.
// flush() marks the end of one radio burst.
//
// OSD rows: event 0, heat 2, status 5 (stage/GO/finish), lap result 15.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lap_frame.h"

namespace race
{

enum Mode : uint8_t
{
    PRACTICE = 0,
    RACE = 1,
};

enum Stage : uint8_t
{
    IDLE = 0,
    PRACTICE_RUN = 1,
    STAGING = 2,
    RACING = 3,
    FINISHED = 4,
};

enum Limit : uint8_t
{
    LIMIT_LAPS = 0,
    LIMIT_TIME = 1,
};

constexpr uint8_t ROW_EVENT = 0;
constexpr uint8_t ROW_HEAT = 2;
constexpr uint8_t ROW_STATUS = 5;
constexpr uint8_t ROW_LAP = 15;

constexpr size_t EVENT_MAX = 40;
constexpr size_t PREFIX_MAX = 16;
constexpr size_t MSG_MAX = 16;

// Which race messages go out, and their wording. Lap frames always go to the
// app; these toggles only cover the text messages.
struct Messages
{
    bool event;       // event name on row 0
    bool heat;        // "HEAT 4" on row 2
    bool startFinish; // stage / GO / finish on row 5 (race mode only)
    bool laps;        // each lap on row 15
    bool best;        // " | BEST" on a new best lap
    char stage[MSG_MAX + 1];
    char go[MSG_MAX + 1];
    char finish[MSG_MAX + 1];
};

inline Messages defaultMessages()
{
    Messages m = {true, true, true, true, true, "ARM NOW", "GO!", "FINISHED!"};
    return m;
}

// What one session runs with.
struct Setup
{
    Mode mode;
    Limit limit;
    uint8_t laps;    // race lap limit (LIMIT_LAPS)
    uint16_t timeS;  // race time limit (LIMIT_TIME)
    uint16_t heat;
    uint8_t pilot;
    const char *event;
    const char *heatPrefix;
    Messages msg;
};

class Sink
{
public:
    virtual ~Sink() = default;
    virtual void frame(const lapframe::Frame &f) = 0; // USB always, BLE if selected
    virtual void osd(uint8_t row, const char *text) = 0; // ESP-NOW if selected
    virtual void osdClear() = 0;
    virtual void flush() = 0; // end of a burst
};

// "0:14.630"
inline void formatTime(uint32_t ms, char *out, size_t len)
{
    snprintf(out, len, "%lu:%02lu.%03lu", (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60),
             (unsigned long)(ms % 1000));
}

// Row 15 text: "L1:0:14.630" or "L1:0:14.630 | BEST"
inline void lapText(uint8_t lap, uint32_t ms, bool best, char *out, size_t len)
{
    char t[16];
    formatTime(ms, t, sizeof(t));
    snprintf(out, len, best ? "L%u:%s | BEST" : "L%u:%s", lap, t);
}

// HDZero's OSD font is upper case only.
inline void upper(const char *in, char *out, size_t len)
{
    size_t i = 0;
    for (; in[i] && i + 1 < len; i++) out[i] = (in[i] >= 'a' && in[i] <= 'z') ? (char)(in[i] - 32) : in[i];
    out[i] = '\0';
}

class Session
{
public:
    explicit Session(Sink &sink) : sink_(sink) {}

    Stage stage() const { return stage_; }
    bool armed() const { return stage_ != IDLE; }
    Mode mode() const { return setup_.mode; }
    uint16_t heat() const { return setup_.heat; }
    uint8_t lapsDone() const { return lapsDone_; }
    uint8_t ignoredCount() const { return ignored_; }
    uint32_t bestMs() const { return bestMs_; }

    // holdMs: STAGING -> GO delay, counted from the first tick() after arming
    // (so the arm burst's airtime doesn't eat into it).
    void arm(const Setup &s, uint32_t holdMs)
    {
        setup_ = s;
        stage_ = s.mode == RACE ? STAGING : PRACTICE_RUN;
        holdMs_ = holdMs;
        goScheduled_ = false;
        started_ = false;
        timeUp_ = false;
        lapsDone_ = 0;
        bestMs_ = 0;
        ignored_ = 0;

        lapframe::Frame f = base(lapframe::ARMED);
        f.mode = s.mode;
        f.heat = s.heat;
        f.lapLimit = s.mode == RACE && s.limit == LIMIT_LAPS ? s.laps : 0;
        f.timeLimitS = s.mode == RACE && s.limit == LIMIT_TIME ? s.timeS : 0;
        sink_.frame(f);
        field(lapframe::SLOT_EVENT, s.event);
        field(lapframe::SLOT_HEAT_PREFIX, s.heatPrefix);

        sink_.osdClear();
        sink_.frame(textFrame(lapframe::SLOT_OSD_CLEAR));
        char buf[64];
        if (s.msg.event && s.event[0])
        {
            upper(s.event, buf, sizeof(buf));
            row(ROW_EVENT, buf);
        }
        if (s.msg.heat)
        {
            char heat[PREFIX_MAX + 8];
            snprintf(heat, sizeof(heat), s.heatPrefix[0] ? "%s %u" : "%s%u", s.heatPrefix, s.heat);
            upper(heat, buf, sizeof(buf));
            row(ROW_HEAT, buf);
        }
        if (s.mode == RACE)
        {
            if (s.msg.startFinish) row(ROW_STATUS, s.msg.stage);
            raceFrame(0);
        }
        sink_.flush();
    }

    void disarm()
    {
        if (!armed()) return;
        stage_ = IDLE;
        lapframe::Frame f = base(lapframe::DISARMED);
        f.heat = setup_.heat;
        f.lapLimit = lapsDone_;
        f.ignored = ignored_;
        sink_.frame(f);
        sink_.flush();
    }

    void tick(uint32_t nowMs)
    {
        if (stage_ == STAGING)
        {
            if (!goScheduled_)
            {
                goScheduled_ = true;
                goMs_ = nowMs + holdMs_;
                return;
            }
            if ((int32_t)(nowMs - goMs_) < 0) return;
            stage_ = RACING;
            goMs_ = nowMs; // the clock starts when GO actually goes out
            if (setup_.msg.startFinish) row(ROW_STATUS, setup_.msg.go);
            raceFrame(0);
            sink_.flush();
        }
        else if (stage_ == RACING && setup_.limit == LIMIT_TIME && !timeUp_ &&
                 nowMs - goMs_ >= (uint32_t)setup_.timeS * 1000)
            timeUp_ = true; // finishes on the next pass
    }

    // A gate pass from the detector, timestamped at the peak.
    void pass(uint32_t passMs)
    {
        if (stage_ == PRACTICE_RUN) practicePass(passMs);
        else if (stage_ == RACING) racePass(passMs);
        // STAGING: false start / sitting at the gate - ignored. FINISHED: done.
    }

    // A peak the detector rejected (lapframe::IGNORED_*). ms: time since the
    // last lap (too soon) or the peak's length (too long). Only counts while
    // the heat is timing - not while staging or after the finish.
    void ignored(uint8_t reason, uint32_t ms)
    {
        if (stage_ != PRACTICE_RUN && stage_ != RACING) return;
        if (ignored_ < 255) ignored_++;
        lapframe::Frame f = base(lapframe::IGNORED);
        f.reason = reason;
        f.ms = ms;
        f.ignored = ignored_;
        sink_.frame(f);
        sink_.flush();
    }

    void heartbeat(uint32_t nowMs)
    {
        lapframe::Frame f = base(lapframe::HEARTBEAT);
        f.stage = stage_;
        f.mode = setup_.mode;
        f.heat = setup_.heat;
        f.ms = nowMs;
        sink_.frame(f);
        sink_.flush();
    }

private:
    Sink &sink_;
    Setup setup_ = {};
    Stage stage_ = IDLE;
    uint8_t seq_ = 0;
    uint32_t holdMs_ = 0;
    bool goScheduled_ = false;
    uint32_t goMs_ = 0;
    bool started_ = false;
    bool timeUp_ = false;
    uint32_t lastPassMs_ = 0;
    uint8_t lapsDone_ = 0;
    uint32_t bestMs_ = 0;
    uint8_t ignored_ = 0;

    lapframe::Frame base(lapframe::Type t)
    {
        lapframe::Frame f = {};
        f.type = t;
        f.seq = seq_++;
        f.pilot = setup_.pilot;
        return f;
    }

    lapframe::Frame textFrame(uint8_t slot)
    {
        lapframe::Frame f = base(lapframe::TEXT);
        f.slot = slot;
        f.parts = 1;
        return f;
    }

    void field(uint8_t slot, const char *text)
    {
        lapframe::Frame frames[lapframe::MAX_TEXT_PARTS];
        size_t n = lapframe::textFrames(slot, text, setup_.pilot, seq_, frames, lapframe::MAX_TEXT_PARTS);
        for (size_t i = 0; i < n; i++) sink_.frame(frames[i]);
    }

    void row(uint8_t r, const char *text)
    {
        sink_.osd(r, text);
        field(r, text);
    }

    void raceFrame(uint32_t raceMs)
    {
        lapframe::Frame f = base(lapframe::RACE);
        f.stage = stage_;
        f.ms = raceMs;
        sink_.frame(f);
    }

    void lapFrame(uint8_t lap, uint8_t flags, uint32_t ms)
    {
        lapframe::Frame f = base(lapframe::LAP);
        f.lap = lap;
        f.flags = flags;
        f.ms = ms;
        sink_.frame(f);
    }

    // Completes a lap ending at passMs. Returns its flags.
    uint8_t completeLap(uint32_t passMs, bool final)
    {
        uint32_t ms = passMs - lastPassMs_;
        lastPassMs_ = passMs;
        lapsDone_++;
        bool best = bestMs_ == 0 || ms < bestMs_;
        if (best) bestMs_ = ms;
        uint8_t flags = (uint8_t)((best ? lapframe::LAP_BEST : 0) | (final ? lapframe::LAP_FINAL : 0));
        lapFrame(lapsDone_, flags, ms);
        if (setup_.msg.laps)
        {
            char text[32];
            lapText(lapsDone_, ms, best && setup_.msg.best, text, sizeof(text));
            row(ROW_LAP, text);
        }
        return flags;
    }

    void practicePass(uint32_t passMs)
    {
        if (!started_)
        {
            started_ = true;
            lastPassMs_ = passMs;
            lapFrame(0, 0, 0);
        }
        else
            completeLap(passMs, false);
        sink_.flush();
    }

    void racePass(uint32_t passMs)
    {
        if ((int32_t)(passMs - goMs_) < 0) return; // peak was before GO
        if (!started_)
        {
            started_ = true;
            lastPassMs_ = passMs;
            lapFrame(0, 0, passMs - goMs_); // holeshot
            sink_.flush();
            return;
        }
        bool final = setup_.limit == LIMIT_LAPS ? lapsDone_ + 1 >= setup_.laps
                                                : timeUp_ || passMs - goMs_ >= (uint32_t)setup_.timeS * 1000;
        completeLap(passMs, final);
        if (final)
        {
            stage_ = FINISHED;
            if (setup_.msg.startFinish) row(ROW_STATUS, setup_.msg.finish);
            raceFrame(passMs - goMs_);
        }
        sink_.flush();
    }
};

} // namespace race
