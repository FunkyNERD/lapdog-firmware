#include "siglog.h"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ~1.3 s at 200 Hz; the printer drains it every 20 ms. Heap, not static: static
// data has to stay clear of the SDR's capture bank (esp_sdr/sram_guard.ld).
static constexpr unsigned SIZE = 256;
static siglog::Entry *ring = nullptr;
static std::atomic<unsigned> head{0}; // written by push() (timing loop)
static std::atomic<unsigned> tail{0}; // written by the printer
static std::atomic<unsigned> dropped{0};
static volatile bool enabled = false;
static TaskHandle_t printer = nullptr;

static void printTask(void *)
{
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
        unsigned d = dropped.exchange(0);
        if (d) printf("L:drop=%u\n", d);
        unsigned t = tail.load();
        while (t != head.load())
        {
            const siglog::Entry e = ring[t % SIZE];
            printf("L:%lu,%d,%.1f,%.1f,%.1f,%u,%u,%u,%c\n", (unsigned long)e.ms, e.gain, e.dbfs, e.baseline,
                   e.above, e.peak, e.clipped, e.since, e.event);
            t++;
            tail.store(t);
        }
    }
}

void siglog::setOn(bool on)
{
    if (on && !ring)
    {
        ring = (Entry *)malloc(SIZE * sizeof(Entry));
        if (!ring)
        {
            printf("L:nomem\n");
            return;
        }
    }
    if (on && !printer)
        // Below the timing loop (6) and the console (5), above the touch UI (3).
        xTaskCreate(printTask, "siglog", 3072, nullptr, 4, &printer);
    if (on)
    {
        tail.store(head.load());
        dropped.store(0);
    }
    enabled = on;
}

bool siglog::on() { return enabled; }

void siglog::push(const Entry &e)
{
    if (!enabled) return;
    unsigned h = head.load();
    if (h - tail.load() >= SIZE)
    {
        dropped++; // printer behind: drop the newest
        return;
    }
    ring[h % SIZE] = e;
    head.store(h + 1);
}
