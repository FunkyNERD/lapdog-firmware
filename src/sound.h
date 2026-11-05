#pragma once
// The touch timer's speaker (ES8311 codec + NS4150B amplifier on the Waveshare board):
// the lap sound on every counted gate pass, as the app's Lap sound setting. Wired audio,
// so it doesn't touch the SDR, BLE or ESP-NOW. Other boards: every call is a no-op.
//
// Nothing here blocks the timing loop. frame() only queues a play request (to the
// timer service task); a short-lived player task with an internal-RAM stack then
// feeds the pre-decoded PCM (22.05 kHz, 16-bit mono, embedded in flash) to I2S, and
// exits - no stack is held while idle (internal RAM is scarce).

#include <stdint.h>

#include "lap_frame.h"
#include "sound_choice.h"

namespace sound
{

// The session's frames, from the timing loop: a sound on each counted pass (the start
// pass or holeshot, and every lap); with cues on, a tone at GO and at the finish.
void frame(const lapframe::Frame &f);

// Plays one sound now at the saved volume, whatever the saved choice (the Sound page's ▶).
// OFF plays nothing. Any task.
void preview(uint8_t choice);

// False once the codec failed to start (no speaker board): the Sound page says so.
bool working();

} // namespace sound
