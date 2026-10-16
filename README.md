# lapdog-firmware

> Sits by the gate. Barks on every lap.

LapDog timer firmware for **ESP32-C5** boards (a mini dev board, or the Waveshare 3.5" touch board): listens on a 5.8 GHz video channel with ESP-SDR, detects the RSSI peak as the drone passes the gate, and reports laps and race messages over USB plus **either** BLE **or** ESP-NOW (VRxC_ELRS OSD on HDZero goggles and LapDog LCD). Design notes are in the project-level `CLAUDE.md`.

**Status: bring-up.** Practice and race sessions, heat numbering, settings, USB, ESP-NOW and the NimBLE advert burst work (tested on the C3). The C5 receive path is ported from ESP-SDR (`src/esp_sdr/`) and captures on the Waveshare touch board (a steady -52 dBFS floor with nothing transmitting, ~150 snapshots a second while armed, clean after every BLE burst); it hasn't seen a quad yet. `sim` still adds synthetic passes on any board.

## Licence

GPL-3.0 (`LICENSE`). The C5 receive path in `src/esp_sdr/` is ported from [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) by ESPARGOS, which is GPL-3.0, so the firmware as a whole is too. `boards/c5_touch/lapdog_touch/` vendors Waveshare's FT6336 touch driver (Apache-2.0, see its `NOTICE`).

## Build

The easiest way onto a board is the browser flasher at https://lapdog.funkynerd.com/flash (Chrome or Edge), which installs the latest release. To build it yourself:

PlatformIO + ESP-IDF via the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform (upstream has no C5 support).

```bash
pio run -e c5_mini -t upload
pio device monitor
pio test -e native
```

### Testing on the 01Space ESP32-C3 0.42" OLED board

`env:c3_oled` builds the same firmware for the 01Space board elrs-race-test runs on. The C3 can't do the SDR (2.4 GHz only), so the capture stub stays and laps come from `sim`, but ESP-NOW, the BLE advert burst, the USB console and the Android app all work as on the C5. It adds a status screen on the board's 72x40 OLED (SSD1306 on I2C, SDA GPIO5, SCL GPIO6), which follows the same frames the app gets:

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

### Waveshare ESP32-C5-Touch-LCD-3.5 (touch timer)

`env:c5_touch` is the timer on Waveshare's 3.5" rounded-corner board (ESP32-C5-WROOM-1U, 8 MB PSRAM, 32 MB flash). The same timer, plus a 480x320 touchscreen (ST7796 over SPI, FT6336 touch and a CH32V003 IO expander for reset and backlight over I2C) running LVGL 9:

| Screen | What |
| --- | --- |
| Idle | event and next heat, Practice / Race, the last session's laps, best and average, **ARM** |
| Armed | lap number and running lap clock, last and best lap, the last three laps, live signal, **HOLD TO DISARM** (1 s) |
| Settings | a tile per category, each showing its current value |
| Channel | band tabs and the band's 8 channels |
| Sensitivity | live signal graph with the enter/exit lines, Low/Med/High, enter and exit in 0.5 dB steps |
| False reads, Radio output, Race messages, Race defaults | the same settings as the app; event name on an on-screen keyboard (capitals only, like the goggles' font) |

Changes save to the timer straight away. Settings are only reachable while idle; arming from anywhere (screen, BOOT, USB) shows the armed screen. The board needs a 5.8 GHz antenna on its U.FL connector. If the picture or touch comes out mirrored, the orientation flags are at the top of `src/touch/board.cpp`.

```bash
pio run -e c5_touch -t upload
```

The board's component (`boards/c5_touch/lapdog_touch/`, with LVGL and the display/touch drivers from the ESP-IDF component registry) is only added to this build.

### SDR (C5)

The receive side of ESP-SDR, ported in `src/esp_sdr/` without its serial protocol: tune, re-measure RX DC at the channel's frequency, fixed gain, and one short raw I/Q snapshot per sample (1024 samples at 20 MS/s through a ~20 MHz analog filter, every 5 ms). `sdr.cpp` turns each snapshot into power in dBFS, and the detector compares that with an in-room baseline it tracks itself (down quickly, up slowly, never during a peak). Wi-Fi stays up on the C5 even with ESP-NOW off, in promiscuous mode as ESP-SDR runs it: it powers and calibrates the PHY, and without promiscuous mode the receive chain is idle and every sample comes back the same.

The capture needs the PHY's RF test library and debug hooks (`sdkconfig.defaults.esp32c5`), two linker wraps, and the 128 KB SRAM bank at 0x40820000 that the modem writes samples into, so all static code and data has to end below it (`src/esp_sdr/sram_guard.ld`; the build fails if it doesn't). ESP-SDR is qualified against the PHY libraries of a newer ESP-IDF than the 5.5.5 this builds with; the functions it calls are all there, but its calibration offsets are unconfirmed until it runs.

Internal RAM is the tight spot: the capture bank takes 128 KB of the C5's 320 KB. So Wi-Fi gets few buffers (it only sends ESP-NOW), and on the touch board LVGL allocates from PSRAM (`lv_mem_psram.c`) and draws through two small internal DMA buffers. The timing loop runs at priority 6, above the screen's drawing, so a busy screen can't slow the sampling on the single-core C5.

Bring-up from the console: `sdr` prints the receiver state (gain, level, last snapshot in dBFS, errors, and captures and time per capture since the last `sdr`), `sdr dump` prints the next snapshot's first raw words, `sdr gain <index|auto>` and `sdr bw <MHz|0>` change the gain and analog bandwidth until the next reboot, and `rssi on` streams the level.

### Flash layout

The C5 mini and C3 have 4 MB flash. The Wi-Fi + NimBLE image is past ESP-IDF's default 1 MB app slot, so `partitions.csv` gives a single ~2 MB app slot (no OTA yet). The touch build (LVGL and fonts, ~1.9 MB) uses `partitions_touch.csv`: 16 MB, one 6 MB app slot.

`vrxc-msp` comes from Forgejo over SSH. To work on it alongside this repo, switch the `lib_deps` line in `platformio.ini` to `symlink://../vrxc-msp`.

### Releases

`tools/release.sh 1.2.3` sets `FIRMWARE_VERSION` in `include/config.h`, commits, tags `v1.2.3` and pushes. The tag runs `.forgejo/workflows/release.yml`: the native tests, then `tools/build-flash-images.py` builds every device in `devices.json` and publishes a Forgejo release with each one's `<id>-firmware.bin` and `<id>-factory.bin` (bootloader, partition table and app merged to flash at 0x0; offsets come from PlatformIO, and the C5's bootloader is at 0x2000), `.sha256` files and a `flash.json`. lapdog-web's `/flash` installs the latest non-prerelease. A suffixed tag (`v1.2.3-beta.1`) is published as a prerelease, which the flasher ignores. The version shows in `get` (`S:fw=`) and the boot log.

The workflow pulls `vrxc-msp` (public) over HTTPS rather than the `ssh://` URL in `platformio.ini`.

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
| `src/sdr.*` | Band power per sample: ESP-SDR snapshots on the C5, a flat floor on the C3, plus `sim` passes |
| `src/esp_sdr/` | The C5 receive path ported from ESP-SDR (GPL-3.0) |
| `src/touch/` | Touch board: display/touch bring-up (`board.cpp`) and the LVGL screens (`ui.cpp`) |
| `boards/c5_touch/` | The touch board's component: LVGL, display and touch drivers (only in `env:c5_touch`) |
| `src/outputs.*` | USB `F:` lines; BLE advert burst (NimBLE) **or** ESP-NOW OSD (via `vrxc-msp`) |
| `src/console.*` | USB commands - full list in `console.h` |
| `src/settings.*`, `button.*`, `led.*` | NVS settings, BOOT long/short press, status LED |

## Modes

- **Long press BOOT (1 s)** toggles idle and timer (arms `set mode practice|race`). **Short press** while armed starts the next heat.
- LED: slow blink = idle, solid = armed, flicker = lap.
- Boots idle (`cfg::BOOT_ARMED`).
- Heartbeats every 5 s while armed. After a disarm it sends idle heartbeats every 2 s for 16 s: DISARMED goes out in one short BLE burst, and an idle timer is otherwise silent, so an app that missed it would keep timing the heat.

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

Each BLE burst gives the frames the app acts on (LAP, ARMED, DISARMED, RACE, IGNORED, HEARTBEAT) most of its airtime and the OSD `TEXT` a short slot each. Then it echoes the key frames 0.7, 1.5 and 2.4 s later: phones only listen part of the time (a Xiaomi on Wi-Fi heard 20-30% of adverts, worst just after the app starts), so a single burst could be missed outright. The app drops the repeats by seq. See `cfg::BLE_*` in `config.h`.

With both on, IDF's Wi-Fi/BT coexistence shares the 2.4 GHz radio and each burst sends ESP-NOW first (the goggles are what the pilot is watching), then BLE - so the app hears a lap ~0.5 s after the goggles. Lap times are timed on the timer, so that delay doesn't change them.

Race messages go to every link that's on. On ESP-NOW they're ELRS Backpack OSD rows in CMRC live timing's format, the same rows and wording elrs-race-test sends and LapDog LCD decodes (the row tells LapDog LCD what a message is):

| Row | Message | Example |
| --- | --- | --- |
| 0 | Event name | `x THURSDAY BACKYARD w` |
| 2 | Heat name | `x HEAT 4 w` |
| 3 | Position & lap being flown (cleared after 3 s) | `P1 \| L3` |
| 5 | Status (cleared after 0.5 s for GO, 2 s for FINISHED) | `w ARM NOW x`, `w GO! x`, `w FINISHED! x` |
| 6–7 | Race result (total from GO; cleared at disarm, or 2 s after a disarm that ends the heat) | `P1` / `T1:02.31` |
| 15 | Lap time & total from the start pass (cleared after 4 s) | `x L2:17.62 \| T35.24 w` |

Text is centred on the 50-column OSD, rows are cleared with 50 NULs, and frames go out 8 ms apart. GO, and each arm, clear the whole OSD. Disarming a heat that's timing sends FINISHED and the result (practice: first pass to last pass; race: GO to last pass), so LapDog LCD ends it but keeps the laps up, and blanks the result again 2 s later with FINISHED. A race that finished at its limit keeps its result up until it's disarmed, which blanks it. Rows are taken down with blank text, never a whole-OSD clear, because LapDog LCD wipes its race panel on an OSD clear but ignores a blank row. `restart` (or disarming before any lap) just clears. On BLE/USB the same row text goes out as LapDog `TEXT` frames (the app drops the `w`/`x` glyph letters), alongside the structured `ARMED`/`LAP`/`RACE` frames.

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
- [ ] A real gate pass with `rssi on`: how far above the floor a quad reads, and pick the default gain and bandwidth
- [ ] Measure SDR ↔ BLE/ESP-NOW switch time
- [ ] Check the NimBLE advert burst with nRF Connect (20 ms interval; falls back to 100 ms if the controller refuses)
- [ ] Measure how long BLE / Wi-Fi init takes when the timer arms with changed links
- [ ] Check SDR capture still works with both BLE and Wi-Fi stacks up (C5)
- [ ] Check the C5 mini's free heap with the SDR's 128 KB bank reserved (no PSRAM there)
