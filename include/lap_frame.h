#pragma once
// The one LapDog message frame, identical on every transport so the Android
// app needs a single parser:
//   USB:  "F:" + hex(frame) + "\n", mixed in with plain-text log/config lines
//   BLE:  raw frame as manufacturer data after the 16-bit company ID
// (ESP-NOW doesn't use this - it speaks VRxC_ELRS OSD so goggles understand it.)
//
// Size budget: a legacy BLE advert carries 31 bytes. Flags take 3 (02 01 06)
// and the manufacturer-data AD header plus company ID take 4, which leaves
// MAX_SIZE = 24 bytes for the frame. Every frame fits that, so one frame is
// one advert. Text longer than one frame's room is chunked (see TEXT).
//
// Header, all types:
//   [0] version (high nibble) | type (low nibble)
//   [1] seq u8      - increments per frame; de-dupes BLE repeats and USB+BLE copies
//   [2] pilot u8
// Body by type (multi-byte values little-endian):
//   LAP       [3] lap u8 (0 = start: practice first pass / race holeshot)
//             [4] flags u8 (LAP_BEST, LAP_FINAL)
//             [5..8] ms u32 (lap time; holeshot = GO to first pass; 0 for the practice start)
//   ARMED     [3] mode u8 (race::Mode)  [4..5] heat u16  [6] lap limit u8 (0 = none)
//             [7..8] time limit s u16 (0 = none)
//   DISARMED  [3..4] heat u16  [5] laps completed u8
//   HEARTBEAT [3] stage u8 (race::Stage)  [4] mode u8  [5..6] heat u16  [7..10] uptime ms u32
//   RACE      [3] stage u8  [4..7] race time ms u32 (since GO; 0 before GO)
//   TEXT      [3] slot u8   [4] part (high nibble) | parts (low nibble)
//             [5..] up to MAX_TEXT_CHUNK bytes of text, no terminator
//     slot 0..17      VRxC_ELRS OSD row: the same text the goggles show there
//                     (empty text = row cleared)
//     SLOT_OSD_CLEAR  whole OSD cleared (no text)
//     SLOT_EVENT      event name, as typed (the timer is the source of truth)
//     SLOT_HEAT_PREFIX heat prefix, as typed
//   A text of up to MAX_TEXT_PARTS * MAX_TEXT_CHUNK bytes goes out as
//   `parts` frames with consecutive seqs; a receiver keeps the latest part
//   set per slot and shows it once all parts have arrived.
//
// Keep in sync with lapdog-android's LapFrame.kt (the tests share vectors).

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace lapframe
{

constexpr uint16_t BLE_COMPANY_ID = 0xFFFF; // test ID until we have a real one
constexpr uint8_t VERSION = 2;
constexpr size_t HEADER = 3;
constexpr size_t MAX_SIZE = 24;
constexpr size_t MAX_TEXT_CHUNK = MAX_SIZE - HEADER - 2; // 19
constexpr size_t MAX_TEXT_PARTS = 15;

enum Type : uint8_t
{
    LAP = 1,
    ARMED = 2,
    DISARMED = 3,
    HEARTBEAT = 4,
    RACE = 5,
    TEXT = 6,
};

constexpr uint8_t LAP_BEST = 0x01;  // fastest lap of this heat so far
constexpr uint8_t LAP_FINAL = 0x02; // this pass finished the race

constexpr uint8_t SLOT_OSD_CLEAR = 0x7F;
constexpr uint8_t SLOT_EVENT = 0x80;
constexpr uint8_t SLOT_HEAT_PREFIX = 0x81;

// One flat struct for every type; encode()/decode() only touch the fields of
// the frame's type.
struct Frame
{
    Type type;
    uint8_t seq;
    uint8_t pilot;
    // LAP
    uint8_t lap;
    uint8_t flags;
    uint32_t ms; // LAP lap time, RACE race time, HEARTBEAT uptime
    // ARMED / DISARMED / HEARTBEAT / RACE
    uint8_t mode;
    uint8_t stage;
    uint16_t heat;
    uint8_t lapLimit; // ARMED; DISARMED: laps completed
    uint16_t timeLimitS;
    // TEXT
    uint8_t slot;
    uint8_t part;
    uint8_t parts;
    uint8_t textLen;
    char text[MAX_TEXT_CHUNK + 1]; // NUL-terminated copy
};

namespace detail
{
inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
inline void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
} // namespace detail

// Body size per type (TEXT: header-only part; the text follows).
inline size_t bodySize(uint8_t type)
{
    switch (type)
    {
    case LAP: return 6;
    case ARMED: return 6;
    case DISARMED: return 3;
    case HEARTBEAT: return 8;
    case RACE: return 5;
    case TEXT: return 2;
    default: return 0;
    }
}

// Writes the frame into out (at least MAX_SIZE bytes). Returns its length,
// 0 for an unknown type.
inline size_t encode(const Frame &f, uint8_t out[MAX_SIZE])
{
    using namespace detail;
    size_t body = bodySize(f.type);
    if (!body) return 0;
    out[0] = (uint8_t)((VERSION << 4) | (f.type & 0x0F));
    out[1] = f.seq;
    out[2] = f.pilot;
    uint8_t *b = out + HEADER;
    switch (f.type)
    {
    case LAP:
        b[0] = f.lap;
        b[1] = f.flags;
        put32(b + 2, f.ms);
        break;
    case ARMED:
        b[0] = f.mode;
        put16(b + 1, f.heat);
        b[3] = f.lapLimit;
        put16(b + 4, f.timeLimitS);
        break;
    case DISARMED:
        put16(b, f.heat);
        b[2] = f.lapLimit;
        break;
    case HEARTBEAT:
        b[0] = f.stage;
        b[1] = f.mode;
        put16(b + 2, f.heat);
        put32(b + 4, f.ms);
        break;
    case RACE:
        b[0] = f.stage;
        put32(b + 1, f.ms);
        break;
    case TEXT:
    {
        size_t n = f.textLen > MAX_TEXT_CHUNK ? MAX_TEXT_CHUNK : f.textLen;
        b[0] = f.slot;
        b[1] = (uint8_t)((f.part << 4) | (f.parts & 0x0F));
        memcpy(b + 2, f.text, n);
        return HEADER + body + n;
    }
    }
    return HEADER + body;
}

inline bool decode(const uint8_t *in, size_t len, Frame &f)
{
    using namespace detail;
    if (len < HEADER || len > MAX_SIZE || (in[0] >> 4) != VERSION) return false;
    uint8_t type = in[0] & 0x0F;
    size_t body = bodySize(type);
    if (!body || len < HEADER + body) return false;
    f = Frame();
    f.type = (Type)type;
    f.seq = in[1];
    f.pilot = in[2];
    const uint8_t *b = in + HEADER;
    switch (type)
    {
    case LAP:
        f.lap = b[0];
        f.flags = b[1];
        f.ms = get32(b + 2);
        break;
    case ARMED:
        f.mode = b[0];
        f.heat = get16(b + 1);
        f.lapLimit = b[3];
        f.timeLimitS = get16(b + 4);
        break;
    case DISARMED:
        f.heat = get16(b);
        f.lapLimit = b[2];
        break;
    case HEARTBEAT:
        f.stage = b[0];
        f.mode = b[1];
        f.heat = get16(b + 2);
        f.ms = get32(b + 4);
        break;
    case RACE:
        f.stage = b[0];
        f.ms = get32(b + 1);
        break;
    case TEXT:
        f.slot = b[0];
        f.part = b[1] >> 4;
        f.parts = b[1] & 0x0F;
        if (f.parts == 0 || f.part >= f.parts) return false;
        f.textLen = (uint8_t)(len - HEADER - body);
        memcpy(f.text, b + 2, f.textLen);
        f.text[f.textLen] = '\0';
        break;
    }
    return true;
}

// Splits text into TEXT frames for slot (always at least one, so an empty
// text clears the slot). seq is advanced per frame. Returns the frame count,
// at most maxFrames; text beyond MAX_TEXT_PARTS chunks is dropped.
inline size_t textFrames(uint8_t slot, const char *text, uint8_t pilot, uint8_t &seq, Frame *out, size_t maxFrames)
{
    size_t len = strlen(text);
    size_t parts = len ? (len + MAX_TEXT_CHUNK - 1) / MAX_TEXT_CHUNK : 1;
    if (parts > MAX_TEXT_PARTS) parts = MAX_TEXT_PARTS;
    if (parts > maxFrames) parts = maxFrames;
    for (size_t i = 0; i < parts; i++)
    {
        Frame &f = out[i];
        f = Frame();
        f.type = TEXT;
        f.seq = seq++;
        f.pilot = pilot;
        f.slot = slot;
        f.part = (uint8_t)i;
        f.parts = (uint8_t)parts;
        size_t from = i * MAX_TEXT_CHUNK;
        size_t n = len > from ? len - from : 0;
        if (n > MAX_TEXT_CHUNK) n = MAX_TEXT_CHUNK;
        f.textLen = (uint8_t)n;
        memcpy(f.text, text + from, n);
        f.text[n] = '\0';
    }
    return parts;
}

} // namespace lapframe
