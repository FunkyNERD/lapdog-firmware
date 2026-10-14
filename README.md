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

- **Practice:** the first gate pass starts lap 1; laps until disarmed.
- **Race:** stage ("ARM NOW") → random 1–3 s hold (`race.hold`) → "GO!" → the first pass is the holeshot → laps → "FINISHED!" at the lap limit, or on the first pass after the time limit.
- The timer numbers heats itself: every arm takes `heat` and advances it (persisted straight away). `restart` reruns the same heat.

### Radio output (exclusive)

One RF chain, so `set link ble|espnow` picks **one**: BLE adverts to the LapDog app, or ESP-NOW OSD to goggles and RacePanel. USB always carries everything. The link is brought up when the timer arms, so a change needs no reboot.

Race messages go to whichever link is selected. On ESP-NOW they're VRxC_ELRS OSD rows: event 0, heat 2, status 5, lap result 15 (e.g. `L1:0:14.630 | BEST`). On BLE/USB they're LapDog `TEXT` frames carrying the same row text, alongside the structured `ARMED`/`LAP`/`RACE` frames.

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
