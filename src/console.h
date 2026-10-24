#pragma once
// Line-based config and control over USB. Commands:
//   get                      print "S:<key>=<value>" lines, then OK
//                            (all of timer_settings::KEYS, plus heat, state, fw)
//   set <key> <value>        keys in timer_settings.h, e.g.
//                              set chan R4 | set freq <MHz>
//                              set enter <dB> | exit <dB> | alpha <0..1>
//                              set minlap <ms>         (1000-30000; shorter passes are ignored)
//                              set park on|off | maxpeak <ms>  (ignore peaks longer than maxpeak)
//                              set link ble|espnow     (exclusive radio output)
//                              set phrase <bind phrase>
//                              set event <name> | set prefix <text> | set heat <next #>
//                              set msg.event|msg.heat|msg.start|msg.laps|msg.best on|off
//                              set text.stage|text.go|text.finish <text>
//                              set mode practice|race  (what BOOT arms)
//                              set race.limit laps|time | race.laps <n> | race.time <s> | race.hold on|off
//   save                     persist to flash
//   defaults                 reset to defaults (not saved)
//   arm practice             start a practice heat (first pass starts lap 1)
//   arm race [laps <n>|time <s>]  stage a race heat (default: saved race limit)
//   disarm                   end the heat
//   heat next                armed: end this heat and start the next one
//   restart                  armed: run this heat again
//   rssi on|off              stream "R:<dB above baseline>" for calibration (idle too)
//   sim                      inject a synthetic gate pass (works armed too)
//   boot                     start-up step timestamps and reset reason, for this boot and
//                            the one before (kept across a reset): S:boot.this|last.<step>=<ms> (+<ms>)
//   sd                       touch timer: the microSD card for website sync (mounts it if
//                            it isn't yet): S:sd=<card>|none, sd.mb, sd.waiting, sd.sent, sd.next
//   net                      touch timer: join a saved Wi-Fi network and fetch the website over
//                            HTTPS, to check website sync can reach it (the result is logged)
//   sdr                      receiver state: S:sdr=on|off|none, sdr.gain, sdr.gainmax,
//                            sdr.bw, sdr.level (dB above baseline), sdr.dbfs (last
//                            snapshot, dB full scale), sdr.errors
//   sdr gain <index>|auto    C5: fixed receive gain index, or the hardware AGC (not saved)
//   sdr bw <MHz>|0           C5: analog bandwidth 11-48 MHz, 0 = widest (not saved)
// Replies OK or ERR [reason]. Setting changes need the timer idle.
// Lap/event frames go out as "F:<hex>" lines - see lap_frame.h.

namespace console
{

void begin(); // starts its own reader task

} // namespace console
