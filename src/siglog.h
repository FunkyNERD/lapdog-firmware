#pragma once
// Full-rate signal log for measuring the receiver (issue #12): "sdr log on" over
// USB. The timing loop push()es one entry per sample into a ring buffer; a
// low-priority task prints them as "L:" lines, so the loop never waits on USB.
// If the host falls behind, new entries are dropped and counted.
//
// L:<ms>,<gain>,<dbfs>,<baseline>,<above>,<peak>,<clipped>,<since>,<event>
//   dbfs      the snapshot's band power, dB full scale (before smoothing)
//   baseline  the detector's in-room baseline (smoothed dB)
//   above     smoothed level - baseline (what the thresholds compare)
//   peak      largest raw |I|/|Q| in the snapshot (511 = full scale)
//   clipped   samples at/above cfg::SDR_CLIP_LEVEL
//   since     captures since the radio was last resumed (0 = right after a burst)
//   event     - none, E peak entered, P pass, S ignored too soon, L ignored too long
// "L:drop=<n>" says n entries were lost.

#include <stdint.h>

namespace siglog
{

struct Entry
{
    uint32_t ms;
    float dbfs, baseline, above;
    int16_t gain;
    uint16_t peak, clipped, since;
    char event;
};

void setOn(bool on);
bool on();
void push(const Entry &e); // timing loop only; cheap, never blocks

} // namespace siglog
