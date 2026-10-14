#pragma once
// Mode state machine shared between the main loop and the console.

namespace app
{

bool armed();
void simulatePass();
void setRssiStream(bool on);

} // namespace app
