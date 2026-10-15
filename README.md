# lapdog-firmware

> Sits by the gate. Barks on every lap.

LapDog timer firmware for a mini **ESP32-C5** board: listens on a 5.8 GHz video channel with ESP-SDR, detects the RSSI peak as the drone passes the gate, and reports laps and race messages over USB plus **either** BLE **or** ESP-NOW (VRxC_ELRS OSD on HDZero goggles and LapDog LCD). Design notes are in the project-level `CLAUDE.md`.

**Status: skeleton.** Practice and race sessions, heat numbering, settings, USB, ESP-NOW and the NimBLE advert burst are written; nothing radio-side has run on hardware yet. The SDR is a stub that returns a noise floor plus synthetic passes (`sim`).

## Build

The easiest way onto a board is the browser flasher at https://lapdog.funkynerd.com/flash (Chrome or Edge), which installs the latest release. To build it yourself:

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

### Testing on the 01Space ESP32-C3 0.42" OLED board

`env:c3_oled` is the same test build for the 01Space board elrs-race-test runs on, with a status screen on its 72x40 OLED (SSD1306 on I2C, SDA GPIO5, SCL GPIO6). The screen follows the same frames the app gets:

| State | Screen |
| --- | --- |
| Idle | channel, `IDLE`, frequency (or the last heat's best lap), radio outputs |
| Armed, before the first pass | heat and mode, `ARMED` (practice) or `READY` / `GO!` (race) |
| Flying | `L3` (or `L3/5` in a race), the running lap time, the last lap (`*` = best) |
| Race finished | `DONE`, best lap, race total |

The screen flashes inverted on every lap. BOOT is GPIO9. The board's LED is a WS2812, which this build doesn't drive. Build with `-DLAPDOG_OLED=0` in place of `-DLAPDOG_OLED=1` to leave the OLED dark.

```bash
pio run -e c3_oled -t upload
pio device monitor -e c3_oled
```

### Flash layout

Both boards have 4 MB flash. The Wi-Fi + NimBLE image is past ESP-IDF's default 1 MB app slot, so `partitions.csv` gives a single ~2 MB app slot (no OTA yet).

`vrxc-msp` comes from Forgejo over SSH. To work on it alongside this repo, switch the `lib_deps` line in `platformio.ini` to `symlink://../vrxc-msp`.

### Releases

`tools/release.sh 1.2.3` sets `FIRMWARE_VERSION` in `include/config.h`, commits, tags `v1.2.3` and pushes. The tag runs `.forgejo/workflows/release.yml`: the native tests, then `tools/build-flash-images.py` builds every device in `devices.json` and publishes a Forgejo release with each one's `<id>-firmware.bin` and `<id>-factory.bin` (bootloader, partition table and app merged to flash at 0x0; offsets come from PlatformIO, and the C5's bootloader is at 0x2000), `.sha256` files and a `flash.json`. lapdog-web's `/flash` installs the latest non-prerelease. A suffixed tag (`v1.2.3-beta.1`) is published as a prerelease, which the flasher ignores. The version shows in `get` (`S:fw=`) and the boot log.

The workflow pulls `vrxc-msp` over HTTPS with the `VRXC_READ_TOKEN` secret (a Forgejo token with `read:repository`), since it's private.

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

- **Practice:** the first gate pass starts lap 1; laps until disarmed. Disarming sends FINISHED and the total (first pass to last pass), like a race finish, so LapDog LCD ends the heat and keeps the laps on screen.
- **Race:** stage ("ARM NOW") → random 1–3 s hold (`race.hold`) → "GO!" → the first pass is the holeshot → laps → "FINISHED!" at the lap limit, or on the first pass after the time limit.
- The timer numbers heats itself: every arm takes `heat` and advances it (persisted straight away). `restart` reruns the same heat.

### False-read protection

- **Minimum lap time** (`set minlap <ms>`, 1–30 s, default 3 s): a peak sooner than this after the last lap is a crash into the gate or a bounce back through it, not a lap.
- **Ignore parked quads** (`set park on|off`, `set maxpeak <ms>`, default on / 1.5 s): a peak longer than this is a quad sitting at the gate; without it, picking the quad up would count a lap.
- Applies in practice and race. Rejected passes don't move the lap reference, and each one is reported as an `IGNORED` frame (reason + count this heat); `DISARMED` carries the heat's total.

### Radio output

`set ble on|off` (BLE adverts to the LapDog app) and `set espnow on|off` (ESP-NOW OSD to goggles and LapDog LCD) are independent: either, both or neither. USB always carries everything. Links are brought up when the timer arms, so a change needs no reboot.

With both on, IDF's Wi-Fi/BT coexistence shares the 2.4 GHz radio and each burst sends ESP-NOW first (the goggles are what the pilot is watching), then BLE - so the app hears a lap ~0.5 s after the goggles. Lap times are timed on the timer, so that delay doesn't change them.

Race messages go to every link that's on. On ESP-NOW they're ELRS Backpack OSD rows in CMRC live timing's format, the same rows and wording elrs-race-test sends and LapDog LCD decodes (the row tells LapDog LCD what a message is):

| Row | Message | Example |
| --- | --- | --- |
| 0 | Event name | `x THURSDAY BACKYARD w` |
| 2 | Heat name | `x HEAT 4 w` |
| 3 | Position & lap being flown | `P1 \| L3` |
| 5 | Status (cleared after 0.5 s for GO, 2 s for FINISHED) | `w ARM NOW x`, `w GO! x`, `w FINISHED! x` |
| 6–7 | Race result (total from GO) | `P1` / `T1:02.31` |
| 15 | Lap time & total from the start pass (cleared after 4 s) | `x L2:17.62 \| T35.24 w` |

Text is centred on the 50-column OSD, rows are cleared with 50 NULs, and frames go out 8 ms apart. GO, and each arm, clear the whole OSD. Disarming a heat that's timing sends FINISHED and the result (practice: first pass to last pass; race: GO to last pass), so LapDog LCD ends it but keeps the laps up; a finished race keeps its results, and `restart` (or disarming before any lap) just clears. On BLE/USB the same row text goes out as LapDog `TEXT` frames (the app drops the `w`/`x` glyph letters), alongside the structured `ARMED`/`LAP`/`RACE` frames.

## USB protocol

Plain text lines. Frames are `F:<hex>` (up to 24 bytes, see `lap_frame.h`), `get` prints `S:<key>=<value>` lines, streamed RSSI is `R:<dB above baseline>`, and every command answers `OK` or `ERR`.

```
set chan R4
set event Thursday Backyard
set ble on
set espnow on
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
- [ ] Measure how long BLE / Wi-Fi init takes when the timer arms with changed links
- [ ] Check SDR capture still works with both BLE and Wi-Fi stacks up (C5)
- [ ] Measure baseline on arm instead of a fixed -90 dB
