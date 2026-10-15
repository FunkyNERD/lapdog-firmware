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
// and as ELRS Backpack OSD rows (ESP-NOW to goggles/LapDog LCD). OSD rows are
// also sent as TEXT frames, so the app shows exactly what the goggles show.
// flush() marks the end of one radio burst.
//
// The OSD messages follow CMRC's live timing (cmrc.asn.au Goggle OSD Layout),
// the same rows and wording elrs-race-test broadcasts and LapDog LCD decodes -
// the row a message is on is what tells LapDog LCD what it is:
//   row 0   event name            "x THURSDAY BACKYARD w"
//   row 2   heat name             "x HEAT 4 w"
//   row 3   position & lap        "P1 | L3"   (the lap now being flown; cleared after 3 s)
//   row 5   status                "w ARM NOW x" / "w GO! x" / "w FINISHED! x"
//   row 6-7 race result           "P1" / "T1:02.31"   (total from GO)
//   row 15  lap time & total      "x L2:17.62 | T35.24 w"   (total from the start pass)
// Times are CMRC's: hundredths, minutes only once there are any. On the HDZero
// font the lone lower-case 'w' and 'x' draw as decorative glyphs. GO, the lap
// line, lap results and FINISHED are cleared again after a while (tick(),
// which keeps running after disarm for that), like the timers do. Rows are
// cleared with blank text, never a full OSD clear: LapDog LCD ignores a blank
// row but wipes its race panel on an OSD clear, so a blank row takes the text
// off the goggles while LapDog LCD keeps what it has. LapDog is a
// single-pilot timer, so the position is always P1.
//
// Ending a heat: a race finishes itself at its limit and keeps its result
// (rows 6-7) up until it's disarmed, which blanks them. Disarming a practice
// heat (or a race still under way) sends the same FINISHED + result, so
// LapDog LCD and the app get them, then blanks them with FINISHED. LapDog LCD
// ends the heat but keeps its laps on screen either way. Disarming before any
// lap has started just clears the OSD.

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
constexpr uint8_t ROW_LAP = 3;         // position & current lap
constexpr uint8_t ROW_STATUS = 5;      // stage / GO / finished
constexpr uint8_t ROW_RESULTS = 6;     // "P1", then the total on the next row
constexpr uint8_t ROW_LAP_RESULT = 15; // lap time & race total

// How long these stay up before the row is cleared again.
constexpr uint32_t GO_UPTIME_MS = 500;
constexpr uint32_t FINISH_UPTIME_MS = 2000;
constexpr uint32_t LAP_RESULT_UPTIME_MS = 4000;
constexpr uint32_t LAP_LINE_UPTIME_MS = 3000;
constexpr uint32_t RESULTS_UPTIME_MS = FINISH_UPTIME_MS; // a disarm's result, rows 6-7

constexpr size_t EVENT_MAX = 40;
constexpr size_t PREFIX_MAX = 16;
constexpr size_t MSG_MAX = 16;

// Which race messages go out, and their wording. Lap frames always go to the
// app; these toggles only cover the text messages.
struct Messages
{
    bool event;       // event name on row 0
    bool heat;        // "HEAT 4" on row 2
    bool startFinish; // stage / GO / FINISHED on row 5, result on rows 6-7 (FINISHED also ends a practice heat on disarm)
    bool laps;        // position & lap on row 3, each lap's time on row 15
    bool best;        // app only: CMRC's lap row has no room for a callout (LAP frames carry LAP_BEST)
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

// "0:14.630" (logs and the console)
inline void formatTime(uint32_t ms, char *out, size_t len)
{
    snprintf(out, len, "%lu:%02lu.%03lu", (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60),
             (unsigned long)(ms % 1000));
}

// CMRC's OSD times: hundredths, minutes only once there are any - "14.63", "1:02.31".
inline void cmrcTime(uint32_t ms, char *out, size_t len)
{
    unsigned long cs = ms / 10;
    if (cs >= 6000) snprintf(out, len, "%lu:%02lu.%02lu", cs / 6000, cs / 100 % 60, cs % 100);
    else snprintf(out, len, "%02lu.%02lu", cs / 100, cs % 100);
}

// Row 15: "x L2:17.62 | T35.24 w" - lap 2's time, and the total since the start pass.
inline void lapResultText(uint8_t lap, uint32_t lapMs, uint32_t totalMs, char *out, size_t len)
{
    char l[16], t[16];
    cmrcTime(lapMs, l, sizeof(l));
    cmrcTime(totalMs, t, sizeof(t));
    snprintf(out, len, "x L%u:%s | T%s w", lap, l, t);
}

// Row 3: "P1 | L3" (lap 3 is being flown).
inline void lapLineText(uint8_t lap, char *out, size_t len) { snprintf(out, len, "P1 | L%u", lap); }

// Status (row 5) and name (rows 0 and 2) decorations, as the timers send them.
inline void statusText(const char *msg, char *out, size_t len) { snprintf(out, len, "w %s x", msg); }
inline void nameText(const char *name, char *out, size_t len) { snprintf(out, len, "x %s w", name); }

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
        clearStatusAt_ = clearLapResultAt_ = clearLapLineAt_ = clearResultsAt_ = 0;
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
        char buf[64], text[72];
        if (s.msg.event && s.event[0])
        {
            upper(s.event, buf, sizeof(buf));
            nameText(buf, text, sizeof(text));
            row(ROW_EVENT, text);
        }
        if (s.msg.heat)
        {
            char heat[PREFIX_MAX + 8];
            snprintf(heat, sizeof(heat), s.heatPrefix[0] ? "%s %u" : "%s%u", s.heatPrefix, s.heat);
            upper(heat, buf, sizeof(buf));
            nameText(buf, text, sizeof(text));
            row(ROW_HEAT, text);
        }
        if (s.mode == RACE)
        {
            if (s.msg.startFinish) status(s.msg.stage);
            raceFrame(0);
        }
        sink_.flush();
    }

    // finish: end the heat on the OSD with FINISHED and its total, cleared
    // again shortly after (LapDog LCD keeps the laps up). false abandons it
    // instead - the OSD is just cleared (a restart of the same heat).
    void disarm(uint32_t nowMs, bool finish = true)
    {
        if (!armed()) return;
        Stage was = stage_;
        stage_ = IDLE;
        lapframe::Frame f = base(lapframe::DISARMED);
        f.heat = setup_.heat;
        f.lapLimit = lapsDone_;
        f.ignored = ignored_;
        sink_.frame(f);
        bool timing = (was == PRACTICE_RUN || was == RACING) && started_;
        if (finish && timing)
        {
            // Total to the last gate pass: from the first pass (practice) or GO (race).
            this->finish(lastPassMs_ - (was == RACING ? goMs_ : startPassMs_), nowMs);
            if (setup_.msg.startFinish) schedule(clearResultsAt_, nowMs + RESULTS_UPTIME_MS);
        }
        else if (finish && was == FINISHED)
        {
            // A race that finished at its limit: its result has been up since,
            // and goes now. Blank rows, so LapDog LCD keeps its panel.
            if (setup_.msg.startFinish) clearResults();
        }
        else
        {
            // Nothing timed yet (or abandoned): wipe the goggles, which also
            // tells LapDog LCD there's no heat.
            clearStatusAt_ = clearLapResultAt_ = clearLapLineAt_ = clearResultsAt_ = 0;
            osdClear();
        }
        sink_.flush();
    }

    void tick(uint32_t nowMs)
    {
        bool cleared = expire(clearStatusAt_, ROW_STATUS, nowMs);
        cleared = expire(clearLapResultAt_, ROW_LAP_RESULT, nowMs) || cleared;
        cleared = expire(clearLapLineAt_, ROW_LAP, nowMs) || cleared;
        if (clearResultsAt_ && (int32_t)(nowMs - clearResultsAt_) >= 0)
        {
            clearResults();
            cleared = true;
        }
        if (cleared) sink_.flush();

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
            if (setup_.msg.startFinish)
            {
                osdClear();
                status(setup_.msg.go);
                schedule(clearStatusAt_, nowMs + GO_UPTIME_MS);
            }
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
    uint32_t clearStatusAt_ = 0;     // 0 = nothing scheduled
    uint32_t clearLapResultAt_ = 0;
    uint32_t clearLapLineAt_ = 0;
    uint32_t clearResultsAt_ = 0;
    uint32_t startPassMs_ = 0;       // practice first pass / race holeshot: lap totals count from here
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

    // Writes (or with "" clears) one OSD row, and mirrors it to the app.
    void row(uint8_t r, const char *text)
    {
        sink_.osd(r, text);
        field(r, text);
    }

    void status(const char *msg)
    {
        char text[MSG_MAX + 8];
        statusText(msg, text, sizeof(text));
        row(ROW_STATUS, text);
    }

    void osdClear()
    {
        sink_.osdClear();
        sink_.frame(textFrame(lapframe::SLOT_OSD_CLEAR));
    }

    static void schedule(uint32_t &at, uint32_t when) { at = when ? when : 1; }

    // Clears row r once its deadline has passed. Returns whether it did.
    bool expire(uint32_t &at, uint8_t r, uint32_t nowMs)
    {
        if (!at || (int32_t)(nowMs - at) < 0) return false;
        at = 0;
        row(r, "");
        return true;
    }

    // Row 3 for the lap starting at passMs; it's only on the goggles briefly
    // (LapDog LCD has the lap number by then and ignores the blank row).
    void lapLine(uint8_t lap, uint32_t passMs)
    {
        if (!setup_.msg.laps) return;
        char text[16];
        lapLineText(lap, text, sizeof(text));
        row(ROW_LAP, text);
        schedule(clearLapLineAt_, passMs + LAP_LINE_UPTIME_MS);
    }

    // Blanks the race result, rows 6-7.
    void clearResults()
    {
        clearResultsAt_ = 0;
        row(ROW_RESULTS, "");
        row(ROW_RESULTS + 1, "");
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
            // The finishing lap's row 3 is cleared by finish() instead.
            if (!final) lapLine((uint8_t)(lapsDone_ + 1), passMs);
            char text[40];
            lapResultText(lapsDone_, ms, passMs - startPassMs_, text, sizeof(text));
            row(ROW_LAP_RESULT, text);
            schedule(clearLapResultAt_, passMs + LAP_RESULT_UPTIME_MS);
        }
        return flags;
    }

    // Heat over for this pilot: "FINISHED!", then place and total on rows 6-7.
    // atMs: when it was sent (FINISHED clears FINISH_UPTIME_MS later).
    void finish(uint32_t totalMs, uint32_t atMs)
    {
        if (setup_.msg.laps)
        {
            clearLapLineAt_ = 0;
            row(ROW_LAP, "");
        }
        if (!setup_.msg.startFinish) return;
        status(setup_.msg.finish);
        schedule(clearStatusAt_, atMs + FINISH_UPTIME_MS);
        row(ROW_RESULTS, "P1");
        char t[16], text[20];
        cmrcTime(totalMs, t, sizeof(t));
        snprintf(text, sizeof(text), "T%s", t);
        row(ROW_RESULTS + 1, text);
    }

    void practicePass(uint32_t passMs)
    {
        if (!started_)
        {
            started_ = true;
            lastPassMs_ = startPassMs_ = passMs;
            lapFrame(0, 0, 0);
            lapLine(1, passMs);
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
            lastPassMs_ = startPassMs_ = passMs;
            lapFrame(0, 0, passMs - goMs_); // holeshot
            lapLine(1, passMs);
            sink_.flush();
            return;
        }
        bool final = setup_.limit == LIMIT_LAPS ? lapsDone_ + 1 >= setup_.laps
                                                : timeUp_ || passMs - goMs_ >= (uint32_t)setup_.timeS * 1000;
        completeLap(passMs, final);
        if (final)
        {
            stage_ = FINISHED;
            finish(passMs - goMs_, passMs);
            raceFrame(passMs - goMs_);
        }
        sink_.flush();
    }
};

} // namespace race
