# lapdog-firmware

> Sits by the gate. Barks on every lap.

LapDog timer firmware for a mini **ESP32-C5** board: listens on a 5.8 GHz video channel with ESP-SDR, detects the RSSI peak as the drone passes the gate, and reports laps over USB, BLE and ESP-NOW (VRxC_ELRS OSD on HDZero goggles and RacePanel). Design notes are in the project-level `CLAUDE.md`.

**Status: skeleton.** The mode, detector, settings, USB and ESP-NOW paths work end to end. The SDR is a stub that returns a noise floor plus synthetic passes (`sim`), and BLE advertising is a TODO.

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
| `include/lap_frame.h` | The single frame format for USB and BLE (shared with the Android app) |
| `src/main.cpp` | Idle/timer state machine, capture loop |
| `src/sdr.*` | **Stub** for ESP-SDR band power |
| `src/outputs.*` | USB `F:` lines, ESP-NOW OSD (via `vrxc-msp`), BLE burst (TODO) |
| `src/console.*` | USB config commands (`get`, `set …`, `save`, `rssi on`, `sim`) |
| `src/settings.*`, `button.*`, `led.*` | NVS settings, BOOT long/short press, status LED |

## Modes

- **Long press BOOT (1 s)** toggles idle and timer. **Short press** starts a new session.
- LED: slow blink = idle, solid = armed, flicker = lap.
- Boots idle (`cfg::BOOT_ARMED`).

## USB protocol

Plain text lines. Lap and event frames are `F:<14 hex chars>` (see `lap_frame.h`), and streamed RSSI is `R:<dB>`.

```
set freq 5800
set phrase my bind phrase
save
sim
F:112A0339300000
```

## TODO

- [ ] Confirm LED GPIO on the mini board (`config.h`)
- [ ] Integrate ESP-SDR capture + fixed gain (`sdr.cpp`)
- [ ] Measure SDR ↔ BLE/ESP-NOW switch time
- [ ] NimBLE non-connectable advert burst (`outputs.cpp`)
- [ ] Measure baseline on arm instead of a fixed -90 dB
