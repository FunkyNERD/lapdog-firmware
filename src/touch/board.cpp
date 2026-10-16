#if LAPDOG_TOUCH

#include "board.h"

#include "custom_io_expander_ch32v003.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft6336.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

// --- Pins (Waveshare BSP, esp32_c5_touch_lcd_3_5.h) -----------------------------------
static constexpr gpio_num_t PIN_I2C_SDA = GPIO_NUM_27;
static constexpr gpio_num_t PIN_I2C_SCL = GPIO_NUM_26;
static constexpr gpio_num_t PIN_LCD_SCLK = GPIO_NUM_6; // shared with the SD card
static constexpr gpio_num_t PIN_LCD_MOSI = GPIO_NUM_7;
static constexpr gpio_num_t PIN_SD_MISO = GPIO_NUM_2;
static constexpr gpio_num_t PIN_LCD_DC = GPIO_NUM_5;
static constexpr gpio_num_t PIN_LCD_CS = GPIO_NUM_8;
static constexpr gpio_num_t PIN_TOUCH_INT = GPIO_NUM_3;
static constexpr spi_host_device_t LCD_SPI = SPI2_HOST;
static constexpr uint32_t LCD_PCLK_HZ = 60 * 1000 * 1000;

// The panel is 320x480 portrait. Turned to landscape; if the picture or the
// touch comes out mirrored on a board, these are the knobs.
static constexpr bool DISPLAY_SWAP_XY = true;
static constexpr bool DISPLAY_MIRROR_X = true;
static constexpr bool DISPLAY_MIRROR_Y = true;
static constexpr bool TOUCH_SWAP_XY = true;
static constexpr bool TOUCH_MIRROR_X = true;
static constexpr bool TOUCH_MIRROR_Y = false;

// IO expander pins: P0 touch reset, P1 LCD reset, P5 power to the panel.
static esp_io_expander_handle_t expander = nullptr;

static i2c_master_bus_handle_t i2cBus()
{
    static i2c_master_bus_handle_t bus = nullptr;
    if (bus) return bus;
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = PIN_I2C_SDA;
    cfg.scl_io_num = PIN_I2C_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) bus = nullptr;
    return bus;
}

static bool expanderInit()
{
    i2c_master_bus_handle_t bus = i2cBus();
    if (!bus || custom_io_expander_new_i2c_ch32v003(bus, CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS, &expander) != ESP_OK)
        return false;
    // Reset pulse on the LCD and touch, then power on - Waveshare's sequence.
    esp_io_expander_set_dir(expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_5,
                            IO_EXPANDER_OUTPUT);
    esp_io_expander_set_dir(expander, IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_INPUT);
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_5, 1);
    return true;
}

void board::backlight(int percent)
{
    if (!expander) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    custom_io_expander_set_pwm(expander, (uint8_t)(percent * 255 / 100));
}

bool board::lock(uint32_t timeoutMs) { return lvgl_port_lock(timeoutMs); }
void board::unlock() { lvgl_port_unlock(); }

lv_display_t *board::begin()
{
    if (!expanderInit())
    {
        ESP_LOGE(TAG, "IO expander not found");
        return nullptr;
    }

    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_SCLK;
    bus.mosi_io_num = PIN_LCD_MOSI;
    bus.miso_io_num = PIN_SD_MISO;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = WIDTH * 40 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t ioCfg = {};
    ioCfg.dc_gpio_num = PIN_LCD_DC;
    ioCfg.cs_gpio_num = PIN_LCD_CS;
    ioCfg.pclk_hz = LCD_PCLK_HZ;
    ioCfg.lcd_cmd_bits = 8;
    ioCfg.lcd_param_bits = 8;
    ioCfg.spi_mode = 0;
    ioCfg.trans_queue_depth = 10;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI, &ioCfg, &io));

    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panelCfg = {};
    panelCfg.reset_gpio_num = GPIO_NUM_NC; // reset is on the IO expander (done above)
    panelCfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panelCfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io, &panelCfg, &panel));
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);

    lvgl_port_cfg_t portCfg = ESP_LVGL_PORT_INIT_CONFIG();
    portCfg.task_priority = 3; // under the timing loop: drawing never delays a pass
    portCfg.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&portCfg));

    lvgl_port_display_cfg_t dispCfg = {};
    dispCfg.io_handle = io;
    dispCfg.panel_handle = panel;
    dispCfg.buffer_size = WIDTH * 40;
    dispCfg.double_buffer = true;
    dispCfg.hres = 320;
    dispCfg.vres = 480;
    dispCfg.monochrome = false;
    dispCfg.color_format = LV_COLOR_FORMAT_RGB565;
    dispCfg.rotation.swap_xy = DISPLAY_SWAP_XY;
    dispCfg.rotation.mirror_x = DISPLAY_MIRROR_X;
    dispCfg.rotation.mirror_y = DISPLAY_MIRROR_Y;
    dispCfg.flags.buff_dma = false;
    dispCfg.flags.buff_spiram = true;
    dispCfg.flags.swap_bytes = true;
    lv_display_t *disp = lvgl_port_add_disp(&dispCfg);
    if (!disp) return nullptr;

    esp_lcd_panel_io_handle_t touchIo = nullptr;
    esp_lcd_panel_io_i2c_config_t touchIoCfg = ESP_LCD_TOUCH_IO_I2C_FT6336_CONFIG();
    touchIoCfg.scl_speed_hz = 400000;
    esp_lcd_touch_handle_t touch = nullptr;
    esp_lcd_touch_config_t touchCfg = {};
    touchCfg.x_max = 320;
    touchCfg.y_max = 480;
    touchCfg.rst_gpio_num = GPIO_NUM_NC; // on the IO expander
    touchCfg.int_gpio_num = PIN_TOUCH_INT;
    touchCfg.flags.swap_xy = TOUCH_SWAP_XY;
    touchCfg.flags.mirror_x = TOUCH_MIRROR_X;
    touchCfg.flags.mirror_y = TOUCH_MIRROR_Y;
    if (esp_lcd_new_panel_io_i2c(i2cBus(), &touchIoCfg, &touchIo) == ESP_OK &&
        esp_lcd_touch_new_i2c_ft6336(touchIo, &touchCfg, &touch) == ESP_OK)
    {
        lvgl_port_touch_cfg_t t = {};
        t.disp = disp;
        t.handle = touch;
        lv_indev_t *indev = lvgl_port_add_touch(&t);
        // A long press is a deliberate act (HOLD TO DISARM), not a slow tap.
        if (indev) lv_indev_set_long_press_time(indev, 1000);
    }
    else
        ESP_LOGE(TAG, "touch panel not found");

    backlight(100);
    return disp;
}

#endif // LAPDOG_TOUCH
