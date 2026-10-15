#pragma once
// The timer's settings and their text form, shared by the USB console
// ("set <key> <value>", and "get" printing "S:<key>=<value>" lines) and the
// app's Settings screen. Pure logic, no IDF - unit-tested natively
// (test/test_settings). NVS persistence is in src/settings.cpp.
//
// The timer is the source of truth for the event name, heat prefix and heat
// number; the app only shows them and writes them back with "set".

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channels.h"
#include "detector.h"
#include "race.h"

// Radio outputs, as bits of Settings::links. USB is always on.
enum Link : uint8_t
{
    LINK_BLE = 1 << 0,    // BLE advert bursts to the LapDog app
    LINK_ESPNOW = 1 << 1, // VRxC_ELRS OSD to goggles / RacePanel
};

struct Settings
{
    uint16_t freqMhz;
    char chan[3]; // "R4", or "" for a non-standard frequency
    uint8_t pilot;
    DetectorParams detector;
    char bindPhrase[64]; // ELRS bind phrase -> ESP-NOW UID
    uint8_t links;       // Link bits: either, both or neither. USB always.
    char event[race::EVENT_MAX + 1];
    char heatPrefix[race::PREFIX_MAX + 1];
    race::Messages msg;
    uint8_t mode;      // race::Mode armed by the BOOT button
    uint8_t raceLimit; // race::Limit
    uint8_t raceLaps;
    uint16_t raceTimeS;
    bool randomHold; // 1-3 s between stage and GO; off = fixed 2 s
    bool ignoreParked; // reject peaks longer than detector.maxPeakMs (a quad parked at the gate)
};

namespace timer_settings
{

// Every key "get" prints, in order.
constexpr const char *KEYS[] = {
    "chan",      "freq",      "pilot",     "enter",      "exit",       "alpha",      "minlap",
    "park",      "maxpeak",
    "ble",       "espnow",    "phrase",    "event",     "prefix",     "msg.event",  "msg.heat",   "msg.start",
    "msg.laps",  "msg.best",  "text.stage", "text.go",   "text.finish", "mode",      "race.limit",
    "race.laps", "race.time", "race.hold",
};
constexpr size_t KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);

inline Settings defaults(uint16_t freqMhz, const DetectorParams &det)
{
    Settings s;
    memset(&s, 0, sizeof(s));
    s.freqMhz = freqMhz;
    channels::labelOf(freqMhz, s.chan);
    s.detector = det;
    s.links = LINK_BLE;
    snprintf(s.heatPrefix, sizeof(s.heatPrefix), "Heat");
    s.msg = race::defaultMessages();
    s.mode = race::PRACTICE;
    s.raceLimit = race::LIMIT_LAPS;
    s.raceLaps = 3;
    s.raceTimeS = 120;
    s.randomHold = true;
    s.ignoreParked = true;
    return s;
}

namespace detail
{
inline bool parseBool(const char *v, bool &out)
{
    if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) out = true;
    else if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) out = false;
    else return false;
    return true;
}
inline bool parseLong(const char *v, long lo, long hi, long &out)
{
    char *end;
    out = strtol(v, &end, 10);
    return *v && !*end && out >= lo && out <= hi;
}
inline bool parseFloat(const char *v, float lo, float hi, float &out)
{
    char *end;
    out = strtof(v, &end);
    return *v && !*end && out >= lo && out <= hi;
}
inline bool copyText(const char *v, char *out, size_t len)
{
    if (strlen(v) >= len || strchr(v, '\n')) return false;
    snprintf(out, len, "%s", v);
    return true;
}
} // namespace detail

// Applies one "set <key> <value>". Returns false for an unknown key or a bad
// value (and leaves s unchanged).
inline bool set(Settings &s, const char *key, const char *v)
{
    using namespace detail;
    if (!key || !v) return false;
    long n;
    float f;
    bool b;
    if (!strcmp(key, "chan"))
    {
        uint16_t mhz = channels::freqOf(v);
        if (!mhz) return false;
        s.freqMhz = mhz;
        s.chan[0] = (char)(v[0] >= 'a' ? v[0] - 32 : v[0]);
        s.chan[1] = v[1];
        s.chan[2] = '\0';
    }
    else if (!strcmp(key, "freq"))
    {
        if (!parseLong(v, 4800, 6000, n)) return false;
        s.freqMhz = (uint16_t)n;
        if (!channels::labelOf(s.freqMhz, s.chan)) s.chan[0] = '\0';
    }
    else if (!strcmp(key, "pilot"))
    {
        if (!parseLong(v, 0, 255, n)) return false;
        s.pilot = (uint8_t)n;
    }
    else if (!strcmp(key, "enter"))
    {
        if (!parseFloat(v, 0.5f, 60, f) || f <= s.detector.exitDb) return false;
        s.detector.enterDb = f;
    }
    else if (!strcmp(key, "exit"))
    {
        if (!parseFloat(v, 0, 60, f) || f >= s.detector.enterDb) return false;
        s.detector.exitDb = f;
    }
    else if (!strcmp(key, "alpha"))
    {
        if (!parseFloat(v, 0.01f, 1, f)) return false;
        s.detector.emaAlpha = f;
    }
    else if (!strcmp(key, "minlap"))
    {
        if (!parseLong(v, 1000, 30000, n)) return false;
        s.detector.minLapMs = (uint32_t)n;
    }
    else if (!strcmp(key, "maxpeak"))
    {
        if (!parseLong(v, 300, 10000, n)) return false;
        s.detector.maxPeakMs = (uint32_t)n;
    }
    else if (!strcmp(key, "ble") || !strcmp(key, "espnow"))
    {
        if (!parseBool(v, b)) return false;
        uint8_t bit = !strcmp(key, "ble") ? LINK_BLE : LINK_ESPNOW;
        s.links = b ? s.links | bit : s.links & ~bit;
    }
    else if (!strcmp(key, "phrase")) return copyText(v, s.bindPhrase, sizeof(s.bindPhrase));
    else if (!strcmp(key, "event")) return copyText(v, s.event, sizeof(s.event));
    else if (!strcmp(key, "prefix")) return copyText(v, s.heatPrefix, sizeof(s.heatPrefix));
    else if (!strcmp(key, "text.stage")) return copyText(v, s.msg.stage, sizeof(s.msg.stage));
    else if (!strcmp(key, "text.go")) return copyText(v, s.msg.go, sizeof(s.msg.go));
    else if (!strcmp(key, "text.finish")) return copyText(v, s.msg.finish, sizeof(s.msg.finish));
    else if (!strcmp(key, "msg.event") || !strcmp(key, "msg.heat") || !strcmp(key, "msg.start") ||
             !strcmp(key, "msg.laps") || !strcmp(key, "msg.best") || !strcmp(key, "race.hold") ||
             !strcmp(key, "park"))
    {
        if (!parseBool(v, b)) return false;
        bool &dst = !strcmp(key, "msg.event")   ? s.msg.event
                    : !strcmp(key, "msg.heat")  ? s.msg.heat
                    : !strcmp(key, "msg.start") ? s.msg.startFinish
                    : !strcmp(key, "msg.laps")  ? s.msg.laps
                    : !strcmp(key, "msg.best")  ? s.msg.best
                    : !strcmp(key, "park")      ? s.ignoreParked
                                                : s.randomHold;
        dst = b;
    }
    else if (!strcmp(key, "mode"))
    {
        if (!strcmp(v, "practice")) s.mode = race::PRACTICE;
        else if (!strcmp(v, "race")) s.mode = race::RACE;
        else return false;
    }
    else if (!strcmp(key, "race.limit"))
    {
        if (!strcmp(v, "laps")) s.raceLimit = race::LIMIT_LAPS;
        else if (!strcmp(v, "time")) s.raceLimit = race::LIMIT_TIME;
        else return false;
    }
    else if (!strcmp(key, "race.laps"))
    {
        if (!parseLong(v, 1, 99, n)) return false;
        s.raceLaps = (uint8_t)n;
    }
    else if (!strcmp(key, "race.time"))
    {
        if (!parseLong(v, 10, 3600, n)) return false;
        s.raceTimeS = (uint16_t)n;
    }
    else return false;
    return true;
}

// Text value of one key, as "set" takes it. Returns false for an unknown key.
inline bool get(const Settings &s, const char *key, char *out, size_t len)
{
    const char *onOff[] = {"off", "on"};
    if (!strcmp(key, "chan")) snprintf(out, len, "%s", s.chan);
    else if (!strcmp(key, "freq")) snprintf(out, len, "%u", s.freqMhz);
    else if (!strcmp(key, "pilot")) snprintf(out, len, "%u", s.pilot);
    else if (!strcmp(key, "enter")) snprintf(out, len, "%.1f", s.detector.enterDb);
    else if (!strcmp(key, "exit")) snprintf(out, len, "%.1f", s.detector.exitDb);
    else if (!strcmp(key, "alpha")) snprintf(out, len, "%.2f", s.detector.emaAlpha);
    else if (!strcmp(key, "minlap")) snprintf(out, len, "%lu", (unsigned long)s.detector.minLapMs);
    else if (!strcmp(key, "maxpeak")) snprintf(out, len, "%lu", (unsigned long)s.detector.maxPeakMs);
    else if (!strcmp(key, "park")) snprintf(out, len, "%s", onOff[s.ignoreParked]);
    else if (!strcmp(key, "ble")) snprintf(out, len, "%s", onOff[(s.links & LINK_BLE) != 0]);
    else if (!strcmp(key, "espnow")) snprintf(out, len, "%s", onOff[(s.links & LINK_ESPNOW) != 0]);
    else if (!strcmp(key, "phrase")) snprintf(out, len, "%s", s.bindPhrase);
    else if (!strcmp(key, "event")) snprintf(out, len, "%s", s.event);
    else if (!strcmp(key, "prefix")) snprintf(out, len, "%s", s.heatPrefix);
    else if (!strcmp(key, "msg.event")) snprintf(out, len, "%s", onOff[s.msg.event]);
    else if (!strcmp(key, "msg.heat")) snprintf(out, len, "%s", onOff[s.msg.heat]);
    else if (!strcmp(key, "msg.start")) snprintf(out, len, "%s", onOff[s.msg.startFinish]);
    else if (!strcmp(key, "msg.laps")) snprintf(out, len, "%s", onOff[s.msg.laps]);
    else if (!strcmp(key, "msg.best")) snprintf(out, len, "%s", onOff[s.msg.best]);
    else if (!strcmp(key, "text.stage")) snprintf(out, len, "%s", s.msg.stage);
    else if (!strcmp(key, "text.go")) snprintf(out, len, "%s", s.msg.go);
    else if (!strcmp(key, "text.finish")) snprintf(out, len, "%s", s.msg.finish);
    else if (!strcmp(key, "mode")) snprintf(out, len, "%s", s.mode == race::RACE ? "race" : "practice");
    else if (!strcmp(key, "race.limit")) snprintf(out, len, "%s", s.raceLimit == race::LIMIT_TIME ? "time" : "laps");
    else if (!strcmp(key, "race.laps")) snprintf(out, len, "%u", s.raceLaps);
    else if (!strcmp(key, "race.time")) snprintf(out, len, "%u", s.raceTimeS);
    else if (!strcmp(key, "race.hold")) snprintf(out, len, "%s", onOff[s.randomHold]);
    else return false;
    return true;
}

// The detector a session runs with: the parked-quad limit only when enabled.
inline DetectorParams detectorParams(const Settings &s)
{
    DetectorParams p = s.detector;
    if (!s.ignoreParked) p.maxPeakMs = 0;
    return p;
}

// The session a "arm" starts with these settings. Strings point into s.
inline race::Setup sessionSetup(const Settings &s, race::Mode mode, uint16_t heat)
{
    race::Setup r = {};
    r.mode = mode;
    r.limit = (race::Limit)s.raceLimit;
    r.laps = s.raceLaps;
    r.timeS = s.raceTimeS;
    r.heat = heat;
    r.pilot = s.pilot;
    r.event = s.event;
    r.heatPrefix = s.heatPrefix;
    r.msg = s.msg;
    return r;
}

// Stage -> GO hold: random 1-3 s, or a fixed 2 s. rnd is any 32-bit random.
inline uint32_t holdMs(const Settings &s, uint32_t rnd) { return s.randomHold ? 1000 + rnd % 2001 : 2000; }

} // namespace timer_settings
