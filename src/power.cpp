#include "power.h"

#if LAPDOG_TOUCH

#include <stdio.h>
#include <stdlib.h>

#include "touch/board.h"

// AXP2101 registers (datasheet; Waveshare's example reads the same through XPowersLib).
static constexpr uint8_t REG_STATUS1 = 0x00;      // bit 5 VBUS good, bit 3 battery present
static constexpr uint8_t REG_STATUS2 = 0x01;      // bits 6:5 battery current direction (01 charging), bits 2:0 charger state
static constexpr uint8_t REG_GAUGE_CTRL = 0x18;   // bit 3 fuel gauge enable
static constexpr uint8_t REG_ADC_ENABLE = 0x30;   // bit 0 battery voltage, bit 2 VBUS voltage
static constexpr uint8_t REG_BATT_VOLTAGE = 0x34; // high 6 bits, then low 8: mV
static constexpr uint8_t REG_BATT_PERCENT = 0xA4;

static power::State current = {};

static bool read1(uint8_t reg, uint8_t &v) { return board::pmuRead(reg, &v, 1); }

void power::begin()
{
    uint8_t v;
    if (!read1(REG_ADC_ENABLE, v)) return;
    board::pmuWrite(REG_ADC_ENABLE, v | 0x01 | 0x04);
    if (read1(REG_GAUGE_CTRL, v)) board::pmuWrite(REG_GAUGE_CTRL, v | 0x08);
    poll();
}

void power::poll()
{
    State s = {};
    uint8_t st1, st2, pct;
    uint8_t mv[2];
    s.valid = read1(REG_STATUS1, st1) && read1(REG_STATUS2, st2);
    if (!s.valid)
    {
        current = {};
        current.percent = -1;
        return;
    }
    s.vbus = st1 & 0x20;
    s.battery = st1 & 0x08;
    s.charging = ((st2 >> 5) & 0x03) == 0x01;
    s.chargerState = st2 & 0x07;
    if (s.battery)
    {
        if (board::pmuRead(REG_BATT_VOLTAGE, mv, 2)) s.mv = ((mv[0] & 0x3F) << 8) | mv[1];
        s.percent = read1(REG_BATT_PERCENT, pct) ? (int8_t)(pct & 0x7F) : -1;
        if (s.percent > 100) s.percent = 100;
    }
    else s.percent = -1;
    // The gauge wobbles by a point or two: hold the shown figure until it has moved
    // properly, so the screen doesn't flicker between 49 % and 50 %.
    if (current.valid && current.battery && s.battery && s.percent >= 0 && current.percent >= 0 &&
        abs(s.percent - current.percent) < 2 && s.percent != 0 && s.percent != 100)
        s.percent = current.percent;
    current = s;
}

power::State power::state() { return current; }

void power::printInfo()
{
    State s = current;
    if (!s.valid)
    {
        printf("S:power=none\n");
        return;
    }
    printf("S:power=ok\nS:power.battery=%d\nS:power.percent=%d\nS:power.mv=%d\nS:power.vbus=%d\nS:power.charging=%d\nS:power.charger=%u\n",
           s.battery, s.percent, s.mv, s.vbus, s.charging, s.chargerState);
}

#else

void power::begin() {}
void power::poll() {}
power::State power::state() { return {false, false, false, false, -1, 0, 0}; }
void power::printInfo() {}

#endif // LAPDOG_TOUCH
