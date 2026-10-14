#include <string.h>
#include <unity.h>
#include <string>

#include "timer_settings.h"

namespace ts = timer_settings;

static const DetectorParams DET = {0.2f, 10.0f, 5.0f, 3000};

void setUp() {}
void tearDown() {}

static std::string value(const Settings &s, const char *key)
{
    char buf[80];
    TEST_ASSERT_TRUE_MESSAGE(ts::get(s, key, buf, sizeof(buf)), key);
    return buf;
}

void test_defaults()
{
    Settings s = ts::defaults(5800, DET);
    TEST_ASSERT_EQUAL_STRING("F4", s.chan);
    TEST_ASSERT_EQUAL_STRING("ble", value(s, "link").c_str());
    TEST_ASSERT_EQUAL_STRING("Heat", s.heatPrefix);
    TEST_ASSERT_EQUAL_STRING("ARM NOW", s.msg.stage);
    TEST_ASSERT_EQUAL(3, s.raceLaps);
}

void test_every_key_roundtrips()
{
    Settings s = ts::defaults(5800, DET);
    for (size_t i = 0; i < ts::KEY_COUNT; i++)
    {
        std::string v = value(s, ts::KEYS[i]);
        Settings t = s;
        if (v.empty()) continue; // empty text (event) is set as ""
        TEST_ASSERT_TRUE_MESSAGE(ts::set(t, ts::KEYS[i], v.c_str()), ts::KEYS[i]);
        TEST_ASSERT_EQUAL_STRING(v.c_str(), value(t, ts::KEYS[i]).c_str());
    }
}

void test_channel_and_freq()
{
    Settings s = ts::defaults(5800, DET);
    TEST_ASSERT_TRUE(ts::set(s, "chan", "r4"));
    TEST_ASSERT_EQUAL(5769, s.freqMhz);
    TEST_ASSERT_EQUAL_STRING("R4", s.chan);
    TEST_ASSERT_TRUE(ts::set(s, "freq", "5801"));
    TEST_ASSERT_EQUAL_STRING("", s.chan);
    TEST_ASSERT_FALSE(ts::set(s, "chan", "Z1"));
    TEST_ASSERT_FALSE(ts::set(s, "freq", "2400"));
}

void test_text_and_toggles()
{
    Settings s = ts::defaults(5800, DET);
    TEST_ASSERT_TRUE(ts::set(s, "event", "Thursday Backyard"));
    TEST_ASSERT_EQUAL_STRING("Thursday Backyard", s.event);
    TEST_ASSERT_TRUE(ts::set(s, "event", ""));
    TEST_ASSERT_EQUAL_STRING("", s.event);
    TEST_ASSERT_FALSE(ts::set(s, "event", "this event name is far too long for the goggles"));
    TEST_ASSERT_TRUE(ts::set(s, "phrase", "my bind phrase"));
    TEST_ASSERT_TRUE(ts::set(s, "msg.best", "off"));
    TEST_ASSERT_FALSE(s.msg.best);
    TEST_ASSERT_FALSE(ts::set(s, "msg.best", "maybe"));
    TEST_ASSERT_TRUE(ts::set(s, "link", "espnow"));
    TEST_ASSERT_EQUAL(LINK_ESPNOW, s.link);
    TEST_ASSERT_FALSE(ts::set(s, "link", "both"));
    TEST_ASSERT_FALSE(ts::set(s, "nope", "1"));
}

void test_thresholds_keep_hysteresis()
{
    Settings s = ts::defaults(5800, DET); // 10 / 5
    TEST_ASSERT_FALSE(ts::set(s, "enter", "4"));
    TEST_ASSERT_FALSE(ts::set(s, "exit", "12"));
    TEST_ASSERT_TRUE(ts::set(s, "exit", "3"));
    TEST_ASSERT_TRUE(ts::set(s, "enter", "6"));
    TEST_ASSERT_EQUAL_FLOAT(6.0f, s.detector.enterDb);
    TEST_ASSERT_FALSE(ts::set(s, "alpha", "0"));
    TEST_ASSERT_FALSE(ts::set(s, "minlap", "abc"));
}

void test_race_settings_and_setup()
{
    Settings s = ts::defaults(5800, DET);
    TEST_ASSERT_TRUE(ts::set(s, "race.limit", "time"));
    TEST_ASSERT_TRUE(ts::set(s, "race.time", "90"));
    TEST_ASSERT_FALSE(ts::set(s, "race.laps", "0"));
    race::Setup r = ts::sessionSetup(s, race::RACE, 7);
    TEST_ASSERT_EQUAL(race::LIMIT_TIME, r.limit);
    TEST_ASSERT_EQUAL(90, r.timeS);
    TEST_ASSERT_EQUAL(7, r.heat);

    for (uint32_t rnd : {0u, 1u, 2000u, 0xFFFFFFFFu})
    {
        uint32_t h = ts::holdMs(s, rnd);
        TEST_ASSERT_TRUE(h >= 1000 && h <= 3000);
    }
    TEST_ASSERT_TRUE(ts::set(s, "race.hold", "off"));
    TEST_ASSERT_EQUAL(2000, ts::holdMs(s, 12345));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_every_key_roundtrips);
    RUN_TEST(test_channel_and_freq);
    RUN_TEST(test_text_and_toggles);
    RUN_TEST(test_thresholds_keep_hysteresis);
    RUN_TEST(test_race_settings_and_setup);
    return UNITY_END();
}
