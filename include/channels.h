#pragma once
// 5.8 GHz FPV band/channel table (R/F/E/A/B/L), so the timer can store the
// channel label the pilot picked ("R4") and not just a frequency that may sit
// in two bands. Same table as the Android app's Channels.kt. Pure logic.

#include <stdint.h>
#include <stdio.h>

namespace channels
{

struct Band
{
    char id;
    const char *name;
    uint16_t mhz[8];
};

constexpr Band BANDS[] = {
    {'R', "Raceband", {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}},
    {'F', "Fatshark", {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}},
    {'E', "Boscam E", {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}},
    {'A', "Boscam A", {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}},
    {'B', "Boscam B", {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}},
    {'L', "Lowband", {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621}},
};
constexpr int BAND_COUNT = sizeof(BANDS) / sizeof(BANDS[0]);

// "R4" / "r4" -> 5769. Returns 0 for an unknown label.
inline uint16_t freqOf(const char *label)
{
    if (!label || !label[0] || !label[1] || label[2]) return 0;
    char b = label[0] >= 'a' ? (char)(label[0] - 32) : label[0];
    int ch = label[1] - '1';
    if (ch < 0 || ch > 7) return 0;
    for (const Band &band : BANDS)
        if (band.id == b) return band.mhz[ch];
    return 0;
}

// 5769 -> "R4" (first band in table order that has it). Returns false if
// the frequency isn't a standard channel.
inline bool labelOf(uint16_t mhz, char out[3])
{
    for (const Band &band : BANDS)
        for (int i = 0; i < 8; i++)
            if (band.mhz[i] == mhz)
            {
                out[0] = band.id;
                out[1] = (char)('1' + i);
                out[2] = '\0';
                return true;
            }
    return false;
}

} // namespace channels
