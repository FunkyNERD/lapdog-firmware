#include "led.h"

#include "config.h"
#include "driver/gpio.h"

static led::Pattern pattern = led::IDLE;
static uint32_t flashUntilMs = 0;

static void write(bool on)
{
    if (cfg::PIN_LED == GPIO_NUM_NC) return;
    gpio_set_level(cfg::PIN_LED, on != cfg::LED_ACTIVE_LOW);
}

void led::begin()
{
    if (cfg::PIN_LED == GPIO_NUM_NC) return; // no plain LED on this board
    gpio_reset_pin(cfg::PIN_LED);
    gpio_set_direction(cfg::PIN_LED, GPIO_MODE_OUTPUT);
    write(false);
}

void led::set(Pattern p) { pattern = p; }

void led::flash(uint32_t nowMs) { flashUntilMs = nowMs + 300; }

void led::update(uint32_t nowMs)
{
    if ((int32_t)(flashUntilMs - nowMs) > 0)
    {
        write((nowMs / 50) & 1); // fast flicker reads clearly against solid
        return;
    }
    switch (pattern)
    {
    case IDLE: write((nowMs % 2000) < 100); break;
    case ARMED: write(true); break;
    }
}
