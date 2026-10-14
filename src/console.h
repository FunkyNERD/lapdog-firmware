#pragma once
// Line-based config over USB (idle mode). Commands:
//   get                      print settings
//   set freq <MHz>           set channel frequency
//   set pilot <n>
//   set enter <dB> | exit <dB> | alpha <0..1> | minlap <ms>
//   set phrase <bind phrase>
//   save                     persist to flash
//   defaults                 reset to defaults (not saved)
//   rssi on|off              stream smoothed band power for calibration
//   sim                      inject a synthetic gate pass (works armed too)
// Lap/event frames go out as "F:<hex>" lines - see lap_frame.h.

namespace console
{

void begin(); // starts its own reader task

} // namespace console
