#include "led.h"

#include "config.h"
#include "driver/gpio.h"

#if LAPDOG_LCD147
#include "led_strip.h"
#endif

static led::Pattern pattern = led::IDLE;
static uint32_t flashUntilMs = 0;

#if LAPDOG_LCD147
// The board's LED is one WS2812: blue blink = idle, green = armed, white flicker = lap.
// Kept dim - it's bright, and the screen is the main readout.
static led_strip_handle_t strip = nullptr;
static bool flashing = false;
static uint32_t shown = ~0u;

static void write(bool on)
{
    if (!strip) return;
    uint32_t rgb = !on ? 0 : flashing ? 0x303030 : pattern == led::ARMED ? 0x003C10 : 0x000A3C;
    if (rgb == shown) return; // the RMT write is not free: only when the colour changes
    shown = rgb;
    led_strip_set_pixel(strip, 0, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
    led_strip_refresh(strip);
}

void led::begin()
{
    led_strip_config_t sc = {};
    sc.strip_gpio_num = cfg::PIN_RGB_LED;
    sc.max_leds = 1;
    sc.led_model = LED_MODEL_WS2812;
    sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rc = {};
    rc.clk_src = RMT_CLK_SRC_DEFAULT;
    rc.resolution_hz = 10 * 1000 * 1000;
    if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK) strip = nullptr;
    write(false);
}
#else
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
#endif

void led::set(Pattern p) { pattern = p; }

void led::flash(uint32_t nowMs) { flashUntilMs = nowMs + 300; }

void led::update(uint32_t nowMs)
{
    if ((int32_t)(flashUntilMs - nowMs) > 0)
    {
#if LAPDOG_LCD147
        flashing = true;
#endif
        write((nowMs / 50) & 1); // fast flicker reads clearly against solid
        return;
    }
#if LAPDOG_LCD147
    flashing = false;
#endif
    switch (pattern)
    {
    case IDLE: write((nowMs % 2000) < 100); break;
    case ARMED: write(true); break;
    }
}
