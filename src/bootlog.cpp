#include "bootlog.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// Kept in RTC memory, which survives a reset (though not a power cut): when
// plugging USB in resets a board that stalled on battery, the stalled boot's
// steps are still there to print as "last boot".
static constexpr int MAX_STEPS = 24;
static constexpr uint32_t MAGIC = 0x4C44424F; // "LDBO"

struct Record
{
    uint32_t magic;
    int count;
    int reset;
    const char *names[MAX_STEPS]; // string literals: same addresses in the same firmware
    uint32_t ms[MAX_STEPS];
};

static RTC_NOINIT_ATTR Record current;
static RTC_NOINIT_ATTR Record last;
static bool started = false;

static void start()
{
    started = true;
    if (current.magic == MAGIC && current.count >= 0 && current.count <= MAX_STEPS) last = current;
    else last.magic = 0;
    current = {};
    current.magic = MAGIC;
    current.reset = (int)esp_reset_reason();
}

void bootlog::mark(const char *step)
{
    if (!started) start();
    // Marked from app_main and from LVGL's task (the first frame, the splash).
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    taskENTER_CRITICAL(&lock);
    if (current.count < MAX_STEPS)
    {
        current.names[current.count] = step;
        current.ms[current.count++] = ms;
    }
    taskEXIT_CRITICAL(&lock);
}

static void printRecord(const char *which, const Record &r)
{
    if (r.magic != MAGIC)
    {
        printf("S:boot.%s=none\n", which);
        return;
    }
    printf("S:boot.%s.reset=%d\n", which, r.reset);
    uint32_t prev = 0;
    for (int i = 0; i < r.count; i++)
    {
        printf("S:boot.%s.%s=%lu ms (+%lu)\n", which, r.names[i], (unsigned long)r.ms[i], (unsigned long)(r.ms[i] - prev));
        prev = r.ms[i];
    }
}

void bootlog::print()
{
    printRecord("this", current);
    printRecord("last", last);
}
