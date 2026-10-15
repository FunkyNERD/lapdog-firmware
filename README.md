# lapdog-firmware

> Sits by the gate. Barks on every lap.

LapDog timer firmware for a mini **ESP32-C5** board: listens on a 5.8 GHz video channel with ESP-SDR, detects the RSSI peak as the drone passes the gate, and reports laps and race messages over USB plus **either** BLE **or** ESP-NOW (VRxC_ELRS OSD on HDZero goggles and RacePanel). Design notes are in the project-level `CLAUDE.md`.

**Status: skeleton.** Practice and race sessions, heat numbering, settings, USB, ESP-NOW and the NimBLE advert burst are written; nothing radio-side has run on hardware yet. The SDR is a stub that returns a noise floor plus synthetic passes (`sim`).

## Build

PlatformIO + ESP-IDF via the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform (upstream has no C5 support).

```bash
pio run -e c5_mini -t upload
pio device monitor
pio test -e native
```

### Testing on an ESP32-C3 SuperMini

`env:c3_test` builds the same firmware for an ESP32-C3 SuperMini. The C3 can't do the SDR (2.4 GHz only), so the capture stub stays and laps come from `sim`, but ESP-NOW, the BLE advert burst, the USB console and the Android app all work as on the C5. BOOT is GPIO9 and the blue LED is GPIO8 (lit when low).

```bash
pio run -e c3_test -t upload
pio device monitor -e c3_test
```

### Flash layout

Both boards have 4 MB flash. The Wi-Fi + NimBLE image is past ESP-IDF's default 1 MB app slot, so `partitions.csv` gives a single ~2 MB app slot (no OTA yet).

`vrxc-msp` comes from Forgejo over SSH. To work on it alongside this repo, switch the `lib_deps` line in `platformio.ini` to `symlink://../vrxc-msp`.

## Layout

| Path | What |
| --- | --- |
| `include/config.h` | Pins, timings, first-boot defaults |
| `include/detector.h` | EMA + enter/exit hysteresis peak detector (pure logic, native-tested) |
| `include/lap_frame.h` | The single frame format for USB and BLE (shared with the Android app), incl. the 31-byte advert budget and text chunking |
| `include/race.h` | Practice/race session state machine and race messages (pure logic, native-tested) |
| `include/timer_settings.h` | Settings struct, `set`/`get` text form, defaults (pure logic, native-tested) |
| `include/channels.h` | R/F/E/A/B/L band table |
| `src/main.cpp` | Idle/timer loop, arm/disarm/heat requests, capture |
| `src/sdr.*` | **Stub** for ESP-SDR band power |
| `src/outputs.*` | USB `F:` lines; BLE advert burst (NimBLE) **or** ESP-NOW OSD (via `vrxc-msp`) |
| `src/console.*` | USB commands - full list in `console.h` |
| `src/settings.*`, `button.*`, `led.*` | NVS settings, BOOT long/short press, status LED |

## Modes

- **Long press BOOT (1 s)** toggles idle and timer (arms `set mode practice|race`). **Short press** while armed starts the next heat.
- LED: slow blink = idle, solid = armed, flicker = lap.
- Boots idle (`cfg::BOOT_ARMED`).

### Sessions

- **Practice:** the first gate pass starts lap 1; laps until disarmed. Disarming sends FINISHED and the total (first pass to last pass), like a race finish, so RacePanel ends the heat and keeps the laps on screen.
- **Race:** stage ("ARM NOW") → random 1–3 s hold (`race.hold`) → "GO!" → the first pass is the holeshot → laps → "FINISHED!" at the lap limit, or on the first pass after the time limit.
- The timer numbers heats itself: every arm takes `heat` and advances it (persisted straight away). `restart` reruns the same heat.

### False-read protection

- **Minimum lap time** (`set minlap <ms>`, 1–30 s, default 3 s): a peak sooner than this after the last lap is a crash into the gate or a bounce back through it, not a lap.
- **Ignore parked quads** (`set park on|off`, `set maxpeak <ms>`, default on / 1.5 s): a peak longer than this is a quad sitting at the gate; without it, picking the quad up would count a lap.
- Applies in practice and race. Rejected passes don't move the lap reference, and each one is reported as an `IGNORED` frame (reason + count this heat); `DISARMED` carries the heat's total.

### Radio output (exclusive)

One RF chain, so `set link ble|espnow` picks **one**: BLE adverts to the LapDog app, or ESP-NOW OSD to goggles and RacePanel. USB always carries everything. The link is brought up when the timer arms, so a change needs no reboot.

Race messages go to whichever link is selected. On ESP-NOW they're ELRS Backpack OSD rows in CMRC live timing's format, the same rows and wording elrs-race-test sends and RacePanel decodes (the row tells RacePanel what a message is):

| Row | Message | Example |
| --- | --- | --- |
| 0 | Event name | `x THURSDAY BACKYARD w` |
| 2 | Heat name | `x HEAT 4 w` |
| 3 | Position & lap being flown | `P1 \| L3` |
| 5 | Status (cleared after 0.5 s for GO, 2 s for FINISHED) | `w ARM NOW x`, `w GO! x`, `w FINISHED! x` |
| 6–7 | Race result (total from GO) | `P1` / `T1:02.31` |
| 15 | Lap time & total from the start pass (cleared after 4 s) | `x L2:17.62 \| T35.24 w` |

Text is centred on the 50-column OSD, rows are cleared with 50 NULs, and frames go out 8 ms apart. GO, and each arm, clear the whole OSD. Disarming a heat that's timing sends FINISHED and the result (practice: first pass to last pass; race: GO to last pass), so RacePanel ends it but keeps the laps up; a finished race keeps its results, and `restart` (or disarming before any lap) just clears. On BLE/USB the same row text goes out as LapDog `TEXT` frames (the app drops the `w`/`x` glyph letters), alongside the structured `ARMED`/`LAP`/`RACE` frames.

## USB protocol

Plain text lines. Frames are `F:<hex>` (up to 24 bytes, see `lap_frame.h`), `get` prints `S:<key>=<value>` lines, streamed RSSI is `R:<dB above baseline>`, and every command answers `OK` or `ERR`.

```
set chan R4
set event Thursday Backyard
set link ble
save
arm race laps 3
sim
F:212A03020139300000
```

## TODO

- [ ] Confirm LED GPIO on the mini board (`config.h`)
- [ ] Integrate ESP-SDR capture + fixed gain (`sdr.cpp`)
- [ ] Measure SDR ↔ BLE/ESP-NOW switch time
- [ ] Check the NimBLE advert burst with nRF Connect (20 ms interval; falls back to 100 ms if the controller refuses)
- [ ] Measure how long BLE / Wi-Fi init takes when the timer arms with a changed link
- [ ] Measure baseline on arm instead of a fixed -90 dB
