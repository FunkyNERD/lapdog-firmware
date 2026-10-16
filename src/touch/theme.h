#pragma once
// The touch screens' palette: the app's HUD colours.

#include <stdint.h>

namespace col
{
constexpr uint32_t BG = 0x0A0C0F, SURFACE = 0x14171C, LINE = 0x262B33, DIVIDER = 0x1B1F26;
constexpr uint32_t TEXT = 0xF3F5F7, SOFT = 0xC9CFD6, MUTED = 0xA0A8B3, CHEVRON = 0x6B7480;
constexpr uint32_t ACCENT = 0x3D9BE9, ON_ACCENT = 0x06121F, SELECTED = 0x0E1C2A, SELECTED_TEXT = 0x8CC4F5;
constexpr uint32_t ACCENT_RING = 0x0F2A40, BEST = 0xC4A1FF, BEST_TINT = 0x1C1729, BEST_EDGE = 0x2C2342;
constexpr uint32_t OK = 0x3DD68C, OK_TINT = 0x0F2A1E, OFF = 0x4A525E;
} // namespace col
