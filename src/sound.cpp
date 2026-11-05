#include "sound.h"

#include "settings.h"

#if LAPDOG_TOUCH

#include <string.h>

#include "touch/board.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "sound";

// Waveshare ESP32-C5-Touch-LCD-3.5 (esp32_c5_touch_lcd_3_5.h): I2S on these pins, no MCLK
// (the ES8311 runs from the bit clock), control over the shared I2C bus.
static constexpr gpio_num_t PIN_SCLK = GPIO_NUM_23, PIN_LRCK = GPIO_NUM_10, PIN_DOUT = GPIO_NUM_25, PIN_DIN = GPIO_NUM_24;
static constexpr uint32_t RATE = 22050;

extern const uint8_t bark_start[] asm("_binary_lap_bark_wav_start");
extern const uint8_t bark_end[] asm("_binary_lap_bark_wav_end");
extern const uint8_t tone_start[] asm("_binary_lap_tone_wav_start");
extern const uint8_t tone_end[] asm("_binary_lap_tone_wav_end");
extern const uint8_t ding_start[] asm("_binary_lap_ding_wav_start");
extern const uint8_t ding_end[] asm("_binary_lap_ding_wav_end");
extern const uint8_t bell_start[] asm("_binary_lap_bell_wav_start");
extern const uint8_t bell_end[] asm("_binary_lap_bell_wav_end");
extern const uint8_t chirp_start[] asm("_binary_lap_chirp_wav_start");
extern const uint8_t chirp_end[] asm("_binary_lap_chirp_wav_end");
extern const uint8_t beep_start[] asm("_binary_lap_double_beep_wav_start");
extern const uint8_t beep_end[] asm("_binary_lap_double_beep_wav_end");

struct Clip
{
    const uint8_t *begin, *end;
};
static const Clip CLIPS[sound::CHOICES] = {
    {bark_start, bark_end}, {tone_start, tone_end}, {ding_start, ding_end}, {bell_start, bell_end},
    {chirp_start, chirp_end}, {beep_start, beep_end}, {nullptr, nullptr},
};

// The PCM of a WAV: after its "data" chunk header. The files are 16-bit mono at RATE (checked at build
// time by tools, and here by the fmt chunk).
static bool pcmOf(const Clip &c, const uint8_t *&pcm, size_t &bytes)
{
    if (!c.begin || c.end - c.begin < 44 || memcmp(c.begin, "RIFF", 4) || memcmp(c.begin + 8, "WAVE", 4)) return false;
    const uint8_t *p = c.begin + 12;
    while (p + 8 <= c.end)
    {
        uint32_t len = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
        if (!memcmp(p, "data", 4))
        {
            pcm = p + 8;
            bytes = len > (size_t)(c.end - pcm) ? (size_t)(c.end - pcm) : len;
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

static i2s_chan_handle_t tx = nullptr;
static esp_codec_dev_handle_t dev = nullptr;
static bool failed = false;
static volatile bool playing = false;
static volatile uint8_t wantClip = 0;
static volatile uint8_t wantVolume = 0;

// First play only: the I2S channel and the codec. Kept after that; opened and closed per sound.
static bool setup()
{
    if (dev) return true;
    if (failed) return false;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Small: the DMA buffers must be internal RAM, which the SDR, radios and display leave little of.
    // 3 x 128 frames is 17 ms of audio, plenty for a player task that does nothing else.
    cc.dma_desc_num = 3;
    cc.dma_frame_num = 128;
    cc.auto_clear = true;
    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE);
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    sc.gpio_cfg.bclk = PIN_SCLK;
    sc.gpio_cfg.ws = PIN_LRCK;
    sc.gpio_cfg.dout = PIN_DOUT;
    sc.gpio_cfg.din = PIN_DIN;
    if (i2s_new_channel(&cc, &tx, nullptr) != ESP_OK || i2s_channel_init_std_mode(tx, &sc) != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s won't start (internal DMA RAM free %u, largest block %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        // Out of memory is not final: drop the half-made channel and try again on the next sound.
        if (tx) i2s_del_channel(tx);
        tx = nullptr;
        return false;
    }
    audio_codec_i2s_cfg_t i2s = {};
    i2s.port = I2S_NUM_0;
    i2s.tx_handle = tx;
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s);
    audio_codec_i2c_cfg_t i2c = {};
    i2c.port = I2C_NUM_0;
    i2c.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c.bus_handle = board::i2cBusHandle();
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    es8311_codec_cfg_t ec = {};
    ec.ctrl_if = ctrl;
    ec.gpio_if = gpio;
    ec.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    ec.pa_pin = -1; // the amplifier's enable isn't a GPIO we know of (see board notes)
    ec.use_mclk = false;
    ec.hw_gain.pa_voltage = 5.0f;
    ec.hw_gain.codec_dac_voltage = 3.3f;
    const audio_codec_if_t *codec = data && ctrl && gpio ? es8311_codec_new(&ec) : nullptr;
    if (codec)
    {
        esp_codec_dev_cfg_t dc = {};
        dc.dev_type = ESP_CODEC_DEV_TYPE_OUT;
        dc.codec_if = codec;
        dc.data_if = data;
        dev = esp_codec_dev_new(&dc);
    }
    if (!dev)
    {
        ESP_LOGE(TAG, "no ES8311 answering: is this the touch board?");
        failed = true;
        return false;
    }
    return true;
}

static void playerMain(void *)
{
    const Clip &c = CLIPS[wantClip < sound::CHOICES ? wantClip : sound::OFF];
    const uint8_t *pcm;
    size_t bytes;
    if (pcmOf(c, pcm, bytes) && setup())
    {
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = 1;
        fs.sample_rate = RATE;
        if (esp_codec_dev_open(dev, &fs) == ESP_CODEC_DEV_OK)
        {
            esp_codec_dev_set_out_vol(dev, wantVolume);
            uint8_t chunk[512];
            for (size_t off = 0; off < bytes; off += sizeof(chunk))
            {
                size_t n = bytes - off < sizeof(chunk) ? bytes - off : sizeof(chunk);
                memcpy(chunk, pcm + off, n);
                if (esp_codec_dev_write(dev, chunk, (int)n) != ESP_CODEC_DEV_OK) break;
            }
            // The last chunk is still in the DMA buffers: let it out before the codec closes.
            vTaskDelay(pdMS_TO_TICKS(60));
            esp_codec_dev_close(dev);
        }
    }
    playing = false;
    vTaskDeleteWithCaps(nullptr);
}

// Runs on the timer service task: starts the player (a task create, which the timing loop shouldn't do).
static void start(void *, uint32_t)
{
    if (playing) return;
    playing = true;
    // Stack in PSRAM: internal RAM has well under 5 KB to spare, and the I2S DMA buffers need it more.
    if (xTaskCreateWithCaps(playerMain, "sound", 4096, nullptr, 2, nullptr, MALLOC_CAP_SPIRAM) != pdPASS)
    {
        playing = false;
        ESP_LOGW(TAG, "no memory for the player task");
    }
}

static void request(uint8_t clip)
{
    uint8_t vol = settings::sound().volume;
    if (clip >= sound::CHOICES || clip == sound::OFF || vol == 0 || playing) return;
    wantClip = clip;
    wantVolume = vol;
    xTimerPendFunctionCall(start, nullptr, 0, 0);
}

void sound::frame(const lapframe::Frame &f)
{
    const settings::Sound &s = settings::sound();
    if (f.type == lapframe::LAP) request(s.choice);
    else if (f.type == lapframe::RACE && s.cues && s.choice != OFF)
    {
        if (f.stage == race::RACING) request(TONE);
        else if (f.stage == race::FINISHED) request(DOUBLE_BEEP);
    }
}

void sound::preview(uint8_t choice) { request(choice); }
bool sound::working() { return !failed; }

#else

void sound::frame(const lapframe::Frame &) {}
void sound::preview(uint8_t) {}
bool sound::working() { return false; }

#endif // LAPDOG_TOUCH
