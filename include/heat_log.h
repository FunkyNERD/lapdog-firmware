#pragma once
// Heats for the website: a Recorder follows the session's frames (the same ones
// USB, BLE and the screen get) and builds each finished heat, by the Android app's
// rules (lapdog-android TimerModel), so the timer's copy and the app's copy of a
// heat carry identical laps and the website can merge them:
//   - ARMED starts a heat; DISARMED ends it. A restart is a DISARMED and an ARMED
//     of the same heat, so the abandoned run is a heat of its own, as in the app.
//   - A heat counts once it has started (the start pass / holeshot) or has
//     ignored passes; only heats with timed laps are uploaded.
// uploadJson() is the body of PUT /api/v1/races/{n} (lapdog-web
// docs/dev/device-api.md), the same fields the app sends. Pure: unit-tested in
// test/test_heat_log.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lap_frame.h"
#include "race.h"

namespace heatlog
{

constexpr int MAX_LAPS = 200;

struct Lap
{
    uint8_t lap;
    uint32_t ms;
};

struct Heat
{
    uint16_t heat;
    uint8_t mode; // race::Mode
    uint8_t lapLimit;
    uint16_t timeLimitS;
    bool started;
    bool finished;      // a race that reached its limit
    int32_t holeshotMs; // race: GO to the first pass; -1 = none
    int32_t raceMs;     // race: GO to the finish, the timer's own figure; -1 = none
    uint8_t ignoredSoon, ignoredLong;
    uint16_t lapCount;
    Lap laps[MAX_LAPS];
    // When it was armed: Unix seconds (0 = the clock wasn't set), plus this boot's
    // id and uptime, so a clock set later in the same boot can still date it.
    int64_t startedAt;
    uint32_t bootId;
    uint32_t upMs;
    char event[race::EVENT_MAX + 1];
    char prefix[race::PREFIX_MAX + 1];
};

inline bool uploadable(const Heat &h) { return h.lapCount > 0; }

class Recorder
{
public:
    // Before each arm: what the frames don't carry whole.
    void context(const char *event, const char *prefix, int64_t unixNow, uint32_t bootId, uint32_t upMs)
    {
        snprintf(ctxEvent_, sizeof(ctxEvent_), "%s", event ? event : "");
        snprintf(ctxPrefix_, sizeof(ctxPrefix_), "%s", prefix ? prefix : "");
        ctxUnix_ = unixNow;
        ctxBoot_ = bootId;
        ctxUp_ = upMs;
    }

    // Feeds one frame; true when it ended a heat worth keeping (then see done()).
    bool frame(const lapframe::Frame &f)
    {
        switch (f.type)
        {
        case lapframe::ARMED:
        {
            bool ended = active_ && end();
            begin(f);
            return ended;
        }
        case lapframe::DISARMED:
            if (!active_) return false;
            // The timer's count wins if it's higher (it always is the full count here).
            if (f.ignored > cur_.ignoredSoon + cur_.ignoredLong)
                cur_.ignoredSoon = (uint8_t)(f.ignored - cur_.ignoredLong);
            return end();
        case lapframe::LAP:
            if (!active_) return false;
            if (f.lap == 0)
            {
                cur_.started = true;
                cur_.holeshotMs = cur_.mode == race::RACE ? (int32_t)f.ms : -1;
            }
            else
            {
                cur_.started = true;
                addLap(f.lap, f.ms);
                if (f.flags & lapframe::LAP_FINAL) cur_.finished = true;
            }
            return false;
        case lapframe::RACE:
            if (active_ && f.stage == race::FINISHED)
            {
                cur_.finished = true;
                cur_.raceMs = (int32_t)f.ms;
            }
            return false;
        case lapframe::IGNORED:
            if (!active_) return false;
            if (f.reason == lapframe::IGNORED_TOO_LONG) cur_.ignoredLong++;
            else cur_.ignoredSoon++;
            if (f.ignored > cur_.ignoredSoon + cur_.ignoredLong)
            {
                if (f.reason == lapframe::IGNORED_TOO_LONG) cur_.ignoredLong = (uint8_t)(f.ignored - cur_.ignoredSoon);
                else cur_.ignoredSoon = (uint8_t)(f.ignored - cur_.ignoredLong);
            }
            return false;
        default: return false;
        }
    }

    const Heat &done() const { return done_; }
    bool active() const { return active_; }

private:
    void begin(const lapframe::Frame &f)
    {
        memset(&cur_, 0, sizeof(cur_));
        cur_.heat = f.heat;
        cur_.mode = f.mode;
        cur_.lapLimit = f.lapLimit;
        cur_.timeLimitS = f.timeLimitS;
        cur_.holeshotMs = -1;
        cur_.raceMs = -1;
        cur_.startedAt = ctxUnix_;
        cur_.bootId = ctxBoot_;
        cur_.upMs = ctxUp_;
        memcpy(cur_.event, ctxEvent_, sizeof(cur_.event));
        memcpy(cur_.prefix, ctxPrefix_, sizeof(cur_.prefix));
        active_ = true;
    }

    bool end()
    {
        active_ = false;
        if (!cur_.started && !(cur_.ignoredSoon + cur_.ignoredLong)) return false;
        memcpy(&done_, &cur_, sizeof(done_));
        return true;
    }

    void addLap(uint8_t lap, uint32_t ms)
    {
        for (uint16_t i = 0; i < cur_.lapCount; i++)
            if (cur_.laps[i].lap == lap)
            {
                cur_.laps[i].ms = ms;
                return;
            }
        if (cur_.lapCount >= MAX_LAPS) return;
        // Laps arrive in order; keep them sorted anyway.
        uint16_t i = cur_.lapCount++;
        while (i > 0 && cur_.laps[i - 1].lap > lap)
        {
            cur_.laps[i] = cur_.laps[i - 1];
            i--;
        }
        cur_.laps[i] = {lap, ms};
    }

    Heat cur_ = {};
    Heat done_ = {};
    bool active_ = false;
    char ctxEvent_[race::EVENT_MAX + 1] = "";
    char ctxPrefix_[race::PREFIX_MAX + 1] = "";
    int64_t ctxUnix_ = 0;
    uint32_t ctxBoot_ = 0, ctxUp_ = 0;
};

namespace detail
{
// Appends to a fixed buffer; remembers if it ran out.
struct Out
{
    char *buf;
    size_t len, n;
    bool ok;
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (!ok) return;
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(buf + n, len - n, fmt, ap);
        va_end(ap);
        if (w < 0 || (size_t)w >= len - n) ok = false;
        else n += (size_t)w;
    }
    // A JSON string, escaped.
    void str(const char *s)
    {
        add("\"");
        for (; *s && ok; s++)
        {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\') add("\\%c", c);
            else if (c < 0x20) add("\\u%04x", c);
            else add("%c", c);
        }
        add("\"");
    }
    void seconds(uint32_t ms) { add("%lu.%03lu", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000)); }
};
} // namespace detail

// "Heat 3 · Race" / "3 · Practice", as the app labels it.
inline void label(const Heat &h, char *out, size_t len)
{
    const char *kind = h.mode == race::RACE ? "Race" : "Practice";
    if (h.prefix[0]) snprintf(out, len, "%s %u \xC2\xB7 %s", h.prefix, h.heat, kind);
    else snprintf(out, len, "%u \xC2\xB7 %s", h.heat, kind);
}

// The race upload body. startedAt overrides h.startedAt (0 = leave it out, and
// the website uses the upload time). Returns its length, or 0 if it didn't fit.
inline size_t uploadJson(const Heat &h, int64_t startedAt, char *buf, size_t len)
{
    detail::Out o = {buf, len, 0, len > 0};
    // The fastest lap; the first on a tie.
    int best = -1;
    uint64_t sum = 0;
    uint16_t count = 0;
    for (int i = 0; i < h.lapCount; i++)
    {
        if (best < 0 || h.laps[i].ms < h.laps[best].ms) best = i;
        sum += h.laps[i].ms;
        if (h.laps[i].lap > count) count = h.laps[i].lap; // BLE gaps aside, the last lap number
    }
    int64_t total = h.raceMs >= 0 ? h.raceMs : (h.holeshotMs >= 0 ? h.holeshotMs + (int64_t)sum : (int64_t)sum);

    char lbl[race::PREFIX_MAX + 24];
    label(h, lbl, sizeof(lbl));
    o.add("{\"lap_count\":%u", count);
    if (total > 0)
    {
        o.add(",\"total_time_s\":");
        o.seconds((uint32_t)total);
    }
    if (best >= 0)
    {
        o.add(",\"best_lap_s\":");
        o.seconds(h.laps[best].ms);
    }
    o.add(",\"label\":");
    o.str(lbl);
    if (h.event[0])
    {
        o.add(",\"event\":");
        o.str(h.event);
    }
    if (startedAt > 0) o.add(",\"started_at\":%lld", (long long)startedAt);
    o.add(",\"laps\":[");
    // Race time at the end of each lap: from GO (holeshot first) in a race, from the
    // start pass in practice; unknown after a gap in the lap numbers.
    bool known = h.mode == race::RACE ? h.holeshotMs >= 0 : true;
    int64_t t = h.holeshotMs >= 0 ? h.holeshotMs : 0;
    int expected = 1;
    for (int i = 0; i < h.lapCount; i++)
    {
        const Lap &l = h.laps[i];
        if (l.lap != expected) known = false;
        expected = l.lap + 1;
        t += l.ms;
        o.add("%s{\"lap\":%u", i ? "," : "", l.lap);
        if (known) o.add(",\"t_ms\":%lld", (long long)t);
        o.add(",\"time_s\":");
        o.seconds(l.ms);
        o.add(",\"best\":%s}", i == best ? "true" : "false");
    }
    o.add("],\"source\":\"timer\",\"mode\":\"%s\",\"timer_heat\":%u", h.mode == race::RACE ? "race" : "practice", h.heat);
    if (h.holeshotMs >= 0) o.add(",\"holeshot_ms\":%ld", (long)h.holeshotMs);
    if (h.raceMs >= 0) o.add(",\"race_ms\":%ld", (long)h.raceMs);
    o.add("}");
    return o.ok ? o.n : 0;
}

} // namespace heatlog
