#pragma once
// Website sync on the touch timer: pairs with a pilot's account on the LapDog
// website and uploads the heats it has recorded (heatstore.h), with the same
// device API as LapDog LCD and the Android app (lapdog-web docs/dev/device-api.md),
// as device kind "timer". Manual only: the Sync button on the touch screen.
//
// The C5 has one radio, and while timing it belongs to the SDR, so a job only runs
// while the timer is disarmed, and the timer can't be armed until it's done (busy()).
// A job leaves promiscuous mode, joins a saved 2.4 GHz network (never 5 GHz: that's
// the video band), does its work, then hands the radio back as it found it.
//
// Jobs run on their own task (stack in PSRAM: TLS needs room), one at a time; the
// screen polls status(). NVS writes (token, networks, numbering) can't be made from
// a PSRAM-stack task, so the timing loop makes them in tick().

#include <stdint.h>

#include "lap_frame.h"

namespace cloud
{

constexpr int MAX_NETWORKS = 5;
constexpr int MAX_SCAN = 16;

struct Network
{
    char ssid[33];
    char pass[65];
};

struct ScanResult
{
    char ssid[33];
    int8_t rssi;
    bool open;
};

enum Job : uint8_t
{
    JOB_NONE,
    JOB_SCAN,   // nearby 2.4 GHz networks
    JOB_PAIR,   // claim a pairing code, then sync
    JOB_SYNC,   // check in, upload every heat not uploaded yet
    JOB_UNPAIR, // tell the website, forget the token
    JOB_CHECK,  // join Wi-Fi and fetch the website's health check (the console's "net")
};

struct Status
{
    Job job;          // the running job, or the last one to finish
    bool running;
    bool ok;          // the last job's result (when !running)
    char message[80]; // progress or result, ready for the screen
    uint16_t done, total;
};

// After settings::begin() and the radio (outputs) are up: loads the pairing and
// networks, and starts the task (which mounts the SD card).
void begin();

// --- From the timing loop -----------------------------------------------------------
// The session's frames: finished heats go to the SD card.
void frame(const lapframe::Frame &f);
// Before each arm: what the frames don't carry whole.
void armContext(const char *event, const char *prefix);
// Writes NVS changes the task has made. Call every loop.
void tick();
// A job has the radio: don't arm or run the SDR.
bool busy();

// --- From the screen ------------------------------------------------------------------
bool paired();
const char *deviceName();
const char *pilotName();
bool sdReady();
int pending(); // heats on the card not uploaded yet (-1 before the card is read)

int networkCount();
Network network(int i);
bool saveNetwork(const char *ssid, const char *pass); // replaces one of the same SSID; false if full
void removeNetwork(int i);

// Starting a job fails (false, the reason in status().message) if one is running,
// the timer is armed, or its preconditions aren't met.
bool startScan();
// The console's "sd": mounts the card if it isn't, and logs what's on it.
void checkSd();
bool startPair(const char *code);
bool startSync();
bool startUnpair();
bool startCheck();

Status status();
int scanCount();
ScanResult scanResult(int i);

} // namespace cloud
