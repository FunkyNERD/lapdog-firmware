#include "button.h"

#include "config.h"
#include "driver/gpio.h"

static bool stable = false;   // debounced pressed state
static bool lastRaw = false;
static uint32_t rawChangedMs = 0;
static uint32_t pressedMs = 0;
static bool longFired = false;

void button::begin()
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << cfg::PIN_BUTTON;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
}

button::Event button::poll(uint32_t nowMs)
{
    bool raw = gpio_get_level(cfg::PIN_BUTTON) == 0; // active low
    if (raw != lastRaw)
    {
        lastRaw = raw;
        rawChangedMs = nowMs;
    }
    if (raw != stable && nowMs - rawChangedMs >= cfg::DEBOUNCE_MS)
    {
        stable = raw;
        if (stable)
        {
            pressedMs = nowMs;
            longFired = false;
        }
        else if (!longFired)
            return SHORT_PRESS;
    }
    // Long press fires while still held, so the LED confirms without letting go.
    if (stable && !longFired && nowMs - pressedMs >= cfg::LONG_PRESS_MS)
    {
        longFired = true;
        return LONG_PRESS;
    }
    return NONE;
}
