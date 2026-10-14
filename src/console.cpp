#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

static void printSettings()
{
    const Settings &s = settings::get();
    printf("freq=%u pilot=%u alpha=%.2f enter=%.1f exit=%.1f minlap=%lu phrase=\"%s\" mode=%s\n", s.freqMhz,
           s.pilot, s.detector.emaAlpha, s.detector.enterDb, s.detector.exitDb,
           (unsigned long)s.detector.minLapMs, s.bindPhrase, app::armed() ? "timer" : "idle");
}

static bool handleSet(char *key, char *val)
{
    Settings &s = settings::get();
    if (!key || !val) return false;
    if (!strcmp(key, "freq")) s.freqMhz = (uint16_t)atoi(val);
    else if (!strcmp(key, "pilot")) s.pilot = (uint8_t)atoi(val);
    else if (!strcmp(key, "enter")) s.detector.enterDb = strtof(val, nullptr);
    else if (!strcmp(key, "exit")) s.detector.exitDb = strtof(val, nullptr);
    else if (!strcmp(key, "alpha")) s.detector.emaAlpha = strtof(val, nullptr);
    else if (!strcmp(key, "minlap")) s.detector.minLapMs = (uint32_t)atol(val);
    else if (!strcmp(key, "phrase")) snprintf(s.bindPhrase, sizeof(s.bindPhrase), "%s", val);
    else return false;
    return true;
}

static void handleLine(char *line)
{
    char *cmd = strtok(line, " ");
    if (!cmd) return;
    bool ok = true;
    if (!strcmp(cmd, "get")) printSettings();
    else if (!strcmp(cmd, "sim")) app::simulatePass();
    else if (!strcmp(cmd, "rssi"))
    {
        char *v = strtok(nullptr, " ");
        app::setRssiStream(v && !strcmp(v, "on"));
    }
    else if (app::armed())
    {
        printf("ERR disarm first\n");
        return;
    }
    else if (!strcmp(cmd, "set"))
    {
        char *key = strtok(nullptr, " ");
        char *val = strtok(nullptr, ""); // rest of line (bind phrases have spaces)
        ok = handleSet(key, val);
    }
    else if (!strcmp(cmd, "save")) ok = settings::save();
    else if (!strcmp(cmd, "defaults")) settings::resetDefaults();
    else ok = false;
    printf(ok ? "OK\n" : "ERR\n");
}

static void task(void *)
{
    char line[128];
    while (true)
    {
        if (!fgets(line, sizeof(line), stdin)) continue;
        line[strcspn(line, "\r\n")] = '\0';
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
    xTaskCreate(task, "console", 4096, nullptr, 5, nullptr);
}
