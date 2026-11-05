#pragma once
// Finished heats on the touch board, until the website has them (see cloud.h). One
// file per heat, as the heatlog::Heat it was recorded as, in the board's own flash
// (a wear-levelled FAT volume in the `storage` partition, mounted at /store):
//   /store/out/H00042.BIN   not uploaded yet
//   /store/sent/H00042.BIN  uploaded
// The number is the timer's own, counting up in the flash. Each file also holds the
// race number the website gave it on its first upload attempt, so a retry sends it
// under the same one.
//
// There is no SD card path: the card shares the display's SPI bus and the card
// driver can't get DMA-capable RAM once the radios are up.
//
// Callable from any task: flash writes can't be made from a task whose stack is in
// PSRAM (cloud.cpp's is), and FAT needs more stack than the timing loop has, so
// every call runs on the store's own task.

#include <stddef.h>
#include <stdint.h>

#include "heat_log.h"

namespace heatstore
{

bool begin(); // mounts the flash volume (formatting it the first time)
bool ready();
void printInfo(); // flash use, heats waiting and sent, as S: lines (the console's "store")

// Saves a finished heat (uploadable ones only). False if the flash is full or failed.
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

struct Usage
{
    uint64_t totalBytes, freeBytes;
    int waiting, sent; // heats not uploaded yet, uploaded
};
Usage usage(); // zeros before begin()
// Free space is low enough that the screen should say so (under 10 %).
bool nearlyFull();

} // namespace heatstore
