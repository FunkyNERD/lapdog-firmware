#pragma once
// Finished heats on the touch board's microSD card, until the website has them
// (see cloud.h). One file per heat, as the heatlog::Heat it was recorded as:
//   /sd/lapdog/out/H00042.BIN   not uploaded yet
//   /sd/lapdog/sent/H00042.BIN  uploaded (kept: the card has room for years)
// The number is the timer's own, counting up on this card. Each file also holds
// the race number the website gave it on its first upload attempt, so a retry
// sends it under the same one.
//
// Only cloud.cpp's task calls these: the SD card shares the display's SPI bus,
// and the timing loop's stack is too small for FAT. The SD card's CS is GPIO 9.

#include <stddef.h>
#include <stdint.h>

#include "heat_log.h"

namespace heatstore
{

bool begin(); // mounts the card; false (and stays false) if there isn't one
bool ready();
void printInfo(); // card, heats waiting and sent, as S: lines (the console's "sd")

// Saves a finished heat (uploadable ones only). False if the card failed.
bool save(const heatlog::Heat &h);

struct Pending
{
    uint32_t id;      // the file's number
    int32_t raceNo;   // the website's race number, -1 = none given yet
};

// Heats not uploaded yet, oldest first. Returns how many (up to max).
int pending(Pending *out, int max);
int pendingCount();

bool load(uint32_t id, heatlog::Heat &h);
bool setRaceNo(uint32_t id, int32_t raceNo);
bool markSent(uint32_t id);
// Forget every given race number (the website reset its numbering).
void clearRaceNos();
// Move every sent heat back to out (pairing with another account sends them all).
void resend();

} // namespace heatstore
