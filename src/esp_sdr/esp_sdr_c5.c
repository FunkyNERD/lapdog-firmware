// SPDX-License-Identifier: GPL-3.0-only
//
// ESP32-C5 raw I/Q capture - ported from ESP-SDR by ESPARGOS
// (https://github.com/ESPARGOS/esp-sdr, GPL-3.0), commit e2b35dc5:
//   main/families/c5_c6_c61/receiver.c   prepare_rx(), acquire_iq()
//   main/targets/esp32c5/chip.h, tuning.h
//   main/common/rx_recalibration.c, rx_calibration_state.c (C5 parts)
//   main/common/burst_gain.h, rx_bandwidth.h (C5 parts)
// Only the C5 receive path is kept, without the serial burst protocol, so
// LapDog can measure band power on-chip. Upstream notes that parts of its code
// are AI-generated and that these PHY calls and offsets are undocumented,
// qualified against the PHY archives its pinned ESP-IDF ships.
//
// Linker side (src/CMakeLists.txt): --wrap=phy_chip_set_chan_ana,
// --wrap=phy_set_rx_gain_cal_dc and sram_guard.ld.

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C5

#include "esp_sdr_c5.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/lock.h>

#include "esp_private/phy.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"

// --- Capture memory (chip.h) ---------------------------------------------------------
// The modem writes samples into this SRAM bank while adctrig runs, and the C5's
// ownership bit covers the whole 128 KiB bank, so it's kept out of the heap and
// sram_guard.ld keeps static sections below it.
#define SRAM_OWNER_REG 0x60095004u
#define IQ_BUFFER ((uint32_t *)0x40830000)
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, c5_rf_dump);

// --- Private PHY / RF test library entry points -----------------------------------
extern void adctrig(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern void phy_set_chanfreq(unsigned mhz, unsigned mode);
extern void phy_stop_tx_tone(unsigned);
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_rx_on(unsigned);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_set_rxclk_en(unsigned);
extern unsigned phy_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void phy_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);
extern void force_rx_gain(unsigned, unsigned, unsigned);
extern unsigned char phy_param[];

// Required by the stock RF test archive; no shell is exposed.
int cmd_parse(char *cmd, char *name, int *argc, char **argv)
{
    (void)cmd;
    (void)name;
    (void)argc;
    (void)argv;
    return -1;
}

static unsigned frequency_mhz = 5800;
static bool rx_ready;
static unsigned rx_channel_mode; // C5 PHY channel mode: 1 = wide analog path (rx_bandwidth.h)
static int rx_analog_filter = -1; // -1 keeps the PHY-calibrated capacitance
#define RX_FILTER_REG 6u

// --- Receive DC recalibration (rx_recalibration.c, rx_calibration_state.c) ---------

static unsigned measurement_mhz;
static atomic_uint dc_generation;
static unsigned calibrated_generation;
static bool calibrated;

#define GAIN_REG 0x600a702cu

// A previous manual gain must not override the measurement's gain sweep.
static void release_manual_gain(void) { REG_CLR_BIT(GAIN_REG, 1u << 23); }

// Calibration normally tunes fixed reference channels internally; for the
// duration of our measurement, point it at the frequency being received.
extern void __real_phy_chip_set_chan_ana(unsigned);
void __wrap_phy_chip_set_chan_ana(unsigned mhz) { __real_phy_chip_set_chan_ana(measurement_mhz ? measurement_mhz : mhz); }

// Count DC measurements (the PHY's tracking task may replace the tables), so a
// capture can tell its calibration went stale.
extern void __real_phy_set_rx_gain_cal_dc(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
void __wrap_phy_set_rx_gain_cal_dc(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    __real_phy_set_rx_gain_cal_dc(a, b, c, d);
    atomic_fetch_add(&dc_generation, 1);
}

extern void phy_set_rx_gain_cal_dc(unsigned, unsigned, void *, void *);
extern void phy_adc_rate_cal_rxdc(void);
extern void phy_set_rx_gain_table(unsigned, unsigned);

// Fresh RX DC at mhz, keeping the startup IQ correction (loopback IQ calibration
// would transmit a tone through the antenna).
static void rx_recalibrate_locked(unsigned mhz)
{
    measurement_mhz = mhz;
    release_manual_gain();
    __real_phy_chip_set_chan_ana(mhz);
    unsigned band = phy_param[42];
    uint32_t *flags = (uint32_t *)(void *)(phy_param + 148);
    *flags &= ~0x280u;
    if (band)
    {
        phy_set_rx_gain_cal_dc(0, 1, phy_param + 384, phy_param + 912);
    }
    else
    {
        phy_set_rx_gain_cal_dc(0, 1, phy_param + 312, phy_param + 896);
        phy_adc_rate_cal_rxdc();
        phy_set_rx_gain_cal_dc(1, 1, phy_param + 348, phy_param + 904);
    }
    *flags |= 0x480u;
    phy_set_rx_gain_table(mhz, 0);
    measurement_mhz = 0;
}

static bool rx_recalibration_stale(void)
{
    return !calibrated || atomic_load(&dc_generation) != calibrated_generation;
}

static void rx_recalibrate(unsigned mhz)
{
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    rx_recalibrate_locked(mhz);
    calibrated_generation = atomic_load(&dc_generation);
    calibrated = true;
    _lock_release(&lock);
}

// --- Gain (burst_gain.h) ---------------------------------------------------------------
// Codes are PHY gain indices, not dB. AGCPWR_CTRL7 bits 8..14 hold the maximum
// calibrated index, filled in when the PHY tunes.

static int gain_index = -1;

unsigned esp_sdr_gain_max(void)
{
    unsigned maximum = (REG_READ(GAIN_REG) >> 8) & 127u;
    return maximum < 90u ? maximum : 0u; // never an uncalibrated slot
}

static void gain_apply(void)
{
    bool manual = gain_index >= 0;
    unsigned code = manual ? (unsigned)gain_index : 40u;
    if (code > esp_sdr_gain_max()) code = esp_sdr_gain_max();
    force_rx_gain(manual, code, 0);
}

void esp_sdr_set_gain(int index)
{
    gain_index = index < 0 ? -1 : index;
    if (rx_ready) gain_apply();
}

int esp_sdr_gain(void) { return gain_index; }

// --- Bandwidth (rx_bandwidth.h, C5) ----------------------------------------------------

typedef struct
{
    uint8_t dcap, mhz;
} bw_point_t;

// Mode 1 runs the wide PHY path while keeping the RX0 cap DAC; mode 0 covers 11-23 MHz.
static unsigned bandwidth_phy_mode(unsigned mhz) { return !mhz || mhz > 23u ? 1u : 0u; }

static uint8_t bandwidth_dcap(unsigned mhz)
{
    // Median noise FFTs at 2300/5500 MHz, 80 MS/s IQ10; approximate full widths.
    static const bw_point_t narrow[] = {{0, 23}, {4, 22}, {8, 21}, {12, 20}, {16, 18},
                                        {24, 16}, {32, 15}, {40, 13}, {48, 12}, {60, 11}};
    static const bw_point_t wide[] = {{0, 48}, {4, 45}, {8, 42}, {12, 40}, {16, 37}, {24, 34},
                                      {32, 30}, {40, 27}, {48, 25}, {56, 23}, {60, 22}};
    const bw_point_t *curve = bandwidth_phy_mode(mhz) ? wide : narrow;
    unsigned count = bandwidth_phy_mode(mhz) ? sizeof(wide) / sizeof(wide[0]) : sizeof(narrow) / sizeof(narrow[0]);
    if (!mhz) return 0;
    if (mhz >= curve[0].mhz) return curve[0].dcap;
    if (mhz <= curve[count - 1].mhz) return curve[count - 1].dcap;
    for (unsigned i = 1; i < count; i++)
        if (mhz >= curve[i].mhz)
        {
            unsigned span = curve[i - 1].mhz - curve[i].mhz;
            return curve[i - 1].dcap + ((curve[i].dcap - curve[i - 1].dcap) * (curve[i - 1].mhz - mhz) + span / 2) / span;
        }
    return curve[count - 1].dcap;
}

bool esp_sdr_set_bandwidth(unsigned mhz)
{
    if (mhz && (mhz < 11u || mhz > 48u)) return false;
    rx_analog_filter = mhz ? bandwidth_dcap(mhz) : -1;
    unsigned mode = bandwidth_phy_mode(mhz);
    if (mode != rx_channel_mode)
    {
        rx_channel_mode = mode;
        rx_ready = false; // needs a complete channel setup
    }
    return true;
}

// RX-only analog capacitance, applied around each snapshot and restored after,
// so the PHY's own calibration survives.
static void rx_analog_apply(unsigned saved[2])
{
    for (unsigned j = 0; j < 2; j++)
    {
        saved[j] = phy_chip_i2c_readReg(0x67, 1, RX_FILTER_REG + j);
        if (rx_analog_filter >= 0)
            phy_i2c_writeReg(0x67, 1, RX_FILTER_REG + j, (saved[j] & ~63u) | (unsigned)rx_analog_filter);
    }
}

static void rx_analog_restore(const unsigned saved[2])
{
    if (rx_analog_filter >= 0)
        for (unsigned j = 0; j < 2; j++) phy_i2c_writeReg(0x67, 1, RX_FILTER_REG + j, saved[j]);
}

// --- Receive setup and capture (receiver.c) -------------------------------------------

void esp_sdr_set_frequency(unsigned mhz)
{
    if (mhz < 100u || mhz > 6000u || mhz == frequency_mhz) return;
    frequency_mhz = mhz;
    rx_ready = false;
}

void esp_sdr_invalidate(void) { rx_ready = false; }

void esp_sdr_prepare(void)
{
    static unsigned calibrated_mhz;
    if (rx_ready && !rx_recalibration_stale()) return;
    if (calibrated_mhz != frequency_mhz || rx_recalibration_stale())
    {
        rx_recalibrate(frequency_mhz);
        calibrated_mhz = frequency_mhz;
    }
    // The C5 channel API takes MHz directly.
    phy_set_chanfreq(frequency_mhz, rx_channel_mode);
    phy_stop_tx_tone(1);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
    gain_apply();
    rx_ready = true;
}

bool esp_sdr_capture(unsigned n, unsigned rate, const uint32_t **iq)
{
    if (n < 256u || n > ESP_SDR_MAX_SAMPLES || rate > ESP_SDR_RATE_4) return false;
    esp_sdr_prepare();
    // Sentinels: an incomplete capture leaves the fill pattern, an overrun hits the guard words.
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = 0xa5a0055au;
    for (unsigned j = 0; j < 4; j++) IQ_BUFFER[n + j] = 0x5a5aa5a5u ^ j;
    unsigned analog_saved[2];
    rx_analog_apply(analog_saved);
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    // Vendor selector 0 maps to raw source 15 and pulses the software trigger.
    adctrig(n - 1, 0, 0, rate * 2, 0, 0, 0, 0, 0);
    REG_WRITE(SRAM_OWNER_REG, owner);
    rx_analog_restore(analog_saved);
    for (unsigned j = 0; j < n; j++)
        if (IQ_BUFFER[j] == 0xa5a0055au) return false;
    for (unsigned j = 0; j < 4; j++)
        if (IQ_BUFFER[n + j] != (0x5a5aa5a5u ^ j)) return false;
    *iq = IQ_BUFFER;
    return true;
}

#endif // CONFIG_IDF_TARGET_ESP32C5
