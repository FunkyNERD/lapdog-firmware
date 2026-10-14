#pragma once
// The one LapDog message frame, identical on every transport so the Android
// app needs a single parser:
//   USB:  "F:" + hex(frame) + "\n", mixed in with plain-text log/config lines
//   BLE:  raw frame as manufacturer data after the 16-bit company ID
// (ESP-NOW doesn't use this - it speaks VRxC_ELRS OSD so goggles understand it.)
//
//   [0] version (high nibble) | type (low nibble)
//   [1] seq u8                - increments per frame, for de-duping BLE repeats
//   [2] pilot/channel u8
//   [3..6] value u32 LE       - lap_ms for LAP, uptime_ms otherwise
//
// DRAFT - keep in sync with lapdog-android's LapFrame.kt.

#include <stddef.h>
#include <stdint.h>

namespace lapframe
{

constexpr uint16_t BLE_COMPANY_ID = 0xFFFF; // test ID until we have a real one
constexpr uint8_t VERSION = 1;
constexpr size_t SIZE = 7;

enum Type : uint8_t
{
    LAP = 1,
    ARMED = 2,
    DISARMED = 3,
    HEARTBEAT = 4,
};

struct Frame
{
    Type type;
    uint8_t seq;
    uint8_t pilot;
    uint32_t value;
};

inline void encode(const Frame &f, uint8_t out[SIZE])
{
    out[0] = (uint8_t)((VERSION << 4) | (f.type & 0x0F));
    out[1] = f.seq;
    out[2] = f.pilot;
    for (int i = 0; i < 4; i++) out[3 + i] = (uint8_t)(f.value >> (8 * i));
}

inline bool decode(const uint8_t *in, size_t len, Frame &f)
{
    if (len < SIZE || (in[0] >> 4) != VERSION) return false;
    f.type = (Type)(in[0] & 0x0F);
    f.seq = in[1];
    f.pilot = in[2];
    f.value = (uint32_t)in[3] | ((uint32_t)in[4] << 8) | ((uint32_t)in[5] << 16) | ((uint32_t)in[6] << 24);
    return true;
}

} // namespace lapframe
