#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "bootlog.h"
#include "cloud.h"
#include "config.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "hal/usb_serial_jtag_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdr.h"
#include "settings.h"

static void printSettings()
{
    const Settings &s = settings::get();
    char v[80];
    for (const char *key : timer_settings::KEYS)
        if (timer_settings::get(s, key, v, sizeof(v))) printf("S:%s=%s\n", key, v);
    printf("S:heat=%u\n", settings::nextHeat());
    const char *state = !app::armed() ? "idle" : app::stage() == race::PRACTICE_RUN ? "practice" : "race";
    printf("S:state=%s\n", state);
    printf("S:fw=%s\n", FIRMWARE_VERSION);
}

// USB output can wedge for good: the driver only sends when the hardware says the
// host took the last packet, and ROM prints (the PHY's, during radio start) write to
// the same FIFO behind its back. When a host opens the port right then - an app
// reconnecting after a reset - that signal never comes and every line after it is
// dropped, so "get" goes unanswered. The watchdog notices output not moving while a
// host is attached and flushes the FIFO and re-arms the driver's interrupt.
// What it saw is kept across a reset, for the "boot" command.
struct UsbWatch
{
    uint32_t magic;
    uint32_t kicks, lines;
    uint32_t writable, intraw, intena; // the last sample before a kick
};
static constexpr uint32_t USB_WATCH_MAGIC = 0x4C445553; // "LDUS"
static RTC_NOINIT_ATTR UsbWatch usbNow;
static RTC_NOINIT_ATTR UsbWatch usbLast;

static void watchUsb(void *)
{
    int stuck = 0;
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (usb_serial_jtag_ll_txfifo_writable() || !usb_serial_jtag_is_connected())
        {
            stuck = 0;
            continue;
        }
        // The host polls every millisecond; a second without room is stuck, or
        // nothing on the host is reading (then flushing again does no harm).
        if (++stuck < 4) continue;
        stuck = 0;
        usbNow.writable = usb_serial_jtag_ll_txfifo_writable();
        usbNow.intraw = usb_serial_jtag_ll_get_intraw_mask();
        usbNow.intena = usb_serial_jtag_ll_get_intr_ena_status();
        usbNow.kicks++;
        usb_serial_jtag_ll_txfifo_flush();
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
    }
}

static void printUsbWatch(const char *which, const UsbWatch &w)
{
    if (w.magic != USB_WATCH_MAGIC)
    {
        printf("S:usb.%s=none\n", which);
        return;
    }
    printf("S:usb.%s.kicks=%lu\nS:usb.%s.lines=%lu\n", which, (unsigned long)w.kicks, which, (unsigned long)w.lines);
    if (w.kicks)
        printf("S:usb.%s.fifo=writable %lu, raw 0x%lx, enabled 0x%lx\n", which, (unsigned long)w.writable,
               (unsigned long)w.intraw, (unsigned long)w.intena);
}

// "sdr": receiver state. "sdr gain <index|auto>", "sdr bw <MHz|0>": bring-up
// tuning, applied between captures and not saved.
static bool handleSdr()
{
    char *what = strtok(nullptr, " ");
    char *v = strtok(nullptr, " ");
    if (!what)
    {
        if (!sdr::available())
        {
            printf("S:sdr=none\n");
            return true;
        }
        printf("S:sdr=%s\n", sdr::running() ? "on" : "off");
        if (sdr::gain() < 0) printf("S:sdr.gain=auto\n");
        else printf("S:sdr.gain=%d\n", sdr::gain());
        printf("S:sdr.gainmax=%u\n", sdr::gainMax());
        printf("S:sdr.bw=%u\n", sdr::bandwidth());
        printf("S:sdr.level=%.1f\n", app::signalDb());
        printf("S:sdr.dbfs=%.1f\n", sdr::levelDbfs());
        printf("S:sdr.errors=%lu\n", (unsigned long)sdr::captureErrors());
        uint32_t n, us, recals;
        sdr::stats(n, us, recals);
        printf("S:sdr.captures=%lu\nS:sdr.capture_us=%lu\nS:sdr.recals=%lu\n", (unsigned long)n, (unsigned long)us,
               (unsigned long)recals);
        return true;
    }
    if (!strcmp(what, "dump"))
    {
        sdr::dump();
        return true;
    }
    if (!v || !sdr::available()) return false;
    if (!strcmp(what, "gain"))
    {
        if (!strcmp(v, "auto")) sdr::setGain(-1);
        else
        {
            long n = atol(v);
            if (n < 0 || n > 127) return false;
            sdr::setGain((int)n);
        }
        return true;
    }
    if (!strcmp(what, "bw")) return sdr::setBandwidth((unsigned)atol(v));
    return false;
}

static bool handleArm()
{
    char *what = strtok(nullptr, " ");
    app::ArmRequest r = {};
    if (!what || !strcmp(what, "practice")) r.mode = race::PRACTICE;
    else if (!strcmp(what, "race"))
    {
        r.mode = race::RACE;
        char *kind = strtok(nullptr, " ");
        char *n = strtok(nullptr, " ");
        if (kind)
        {
            if (!n || atol(n) <= 0) return false;
            r.overrideLimit = true;
            if (!strcmp(kind, "laps") && atol(n) <= 99)
            {
                r.limit = race::LIMIT_LAPS;
                r.laps = (uint8_t)atol(n);
            }
            else if (!strcmp(kind, "time") && atol(n) <= 3600)
            {
                r.limit = race::LIMIT_TIME;
                r.timeS = (uint16_t)atol(n);
            }
            else return false;
        }
    }
    else return false;
    app::requestArm(r);
    return true;
}

static void handleLine(char *line)
{
    char *cmd = strtok(line, " ");
    if (!cmd) return;
    bool ok = true;
    if (!strcmp(cmd, "get")) printSettings();
    else if (!strcmp(cmd, "sim")) app::simulatePass();
    else if (!strcmp(cmd, "boot"))
    {
        bootlog::print();
        printUsbWatch("this", usbNow);
        printUsbWatch("last", usbLast);
    }
    else if (!strcmp(cmd, "sd")) cloud::checkSd(); // its S: lines follow, from the sync task
    else if (!strcmp(cmd, "net")) ok = cloud::startCheck(); // the result is logged
    else if (!strcmp(cmd, "sync")) ok = cloud::startSync(); // as SYNC NOW; the result is logged
    else if (!strcmp(cmd, "rssi"))
    {
        char *v = strtok(nullptr, " ");
        app::setRssiStream(v && !strcmp(v, "on"));
    }
    else if (!strcmp(cmd, "disarm")) app::requestDisarm();
    else if (!strcmp(cmd, "sdr")) ok = handleSdr();
    else if (!strcmp(cmd, "heat"))
    {
        char *v = strtok(nullptr, " ");
        if (!v || strcmp(v, "next") || !app::armed()) ok = false;
        else app::requestNewHeat();
    }
    else if (!strcmp(cmd, "restart"))
    {
        if (app::armed()) app::requestRestart();
        else ok = false;
    }
    else if (app::armed())
    {
        printf("ERR disarm first\n");
        return;
    }
    else if (!strcmp(cmd, "arm")) ok = handleArm();
    else if (!strcmp(cmd, "set"))
    {
        char *key = strtok(nullptr, " ");
        char *val = strtok(nullptr, ""); // rest of line (bind phrases, event names have spaces)
        if (!val) val = (char *)"";
        if (key && !strcmp(key, "heat"))
        {
            long n = atol(val);
            ok = n >= 1 && n <= 65535;
            if (ok) settings::setNextHeat((uint16_t)n);
        }
        else ok = timer_settings::set(settings::get(), key, val);
    }
    else if (!strcmp(cmd, "save")) ok = settings::save();
    else if (!strcmp(cmd, "defaults")) settings::resetDefaults();
    else ok = false;
    printf(ok ? "OK\n" : "ERR\n");
}

static void task(void *)
{
    char line[160];
    while (true)
    {
        // With no computer on the USB port (e.g. a cold start on battery), reading
        // stdin can fail at once instead of waiting. Retrying straight away spun
        // this task and starved everything below it, the touch screen included.
        if (!fgets(line, sizeof(line), stdin))
        {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        usbNow.lines++;
        handleLine(line);
    }
}

void console::begin()
{
    // Blocking stdin over USB Serial/JTAG needs the driver, not the default
    // non-blocking VFS.
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&ucfg);
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, nullptr, _IONBF, 0);
    if (usbNow.magic == USB_WATCH_MAGIC) usbLast = usbNow;
    else usbLast.magic = 0;
    usbNow = {};
    usbNow.magic = USB_WATCH_MAGIC;
    xTaskCreate(task, "console", 4096, nullptr, 5, nullptr);
    xTaskCreate(watchUsb, "usbwatch", 2048, nullptr, 2, nullptr);
}
