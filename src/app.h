#pragma once
// Mode state machine shared between the main loop and the console. The
// console runs in its own task, so it only *requests* arm/disarm; the main
// loop owns the session and the radio.

#include <stdint.h>

#include "race.h"

namespace app
{

struct ArmRequest
{
    race::Mode mode;
    bool overrideLimit; // use limit/laps/timeS below instead of the saved race defaults
    race::Limit limit;
    uint8_t laps;
    uint16_t timeS;
};

bool armed();
race::Stage stage();
uint16_t sessionHeat(); // heat of the current/last session

void requestArm(const ArmRequest &r);
void requestDisarm();
void requestNewHeat(); // armed: end this heat, start the next with the same mode
void requestRestart(); // armed: run this heat again from the start
void simulatePass();
void setRssiStream(bool on);

} // namespace app
