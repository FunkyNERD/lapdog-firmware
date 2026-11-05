#pragma once
// The lap sound choices the touch timer shares with the app (lapdog-android
// LapSounds.kt): the names the console takes ("set sound bark") and the stored
// number. Pure logic, unit-tested natively (test/test_settings).

#include <stdint.h>
#include <string.h>

namespace sound
{

enum Choice : uint8_t
{
    BARK,
    TONE,
    DING,
    BELL,
    CHIRP,
    DOUBLE_BEEP,
    OFF,
    CHOICES
};

constexpr const char *NAMES[CHOICES] = {"bark", "tone", "ding", "bell", "chirp", "double", "off"};
constexpr const char *LABELS[CHOICES] = {"Bark", "Tone", "Ding", "Bell", "Chirp", "Double beep", "Off"};

constexpr uint8_t DEFAULT_CHOICE = BARK;
constexpr uint8_t DEFAULT_VOLUME = 70; // percent

// The number for a console name, or -1.
inline int parse(const char *name)
{
    for (int i = 0; i < CHOICES; i++)
        if (!strcmp(name, NAMES[i])) return i;
    return -1;
}

} // namespace sound
