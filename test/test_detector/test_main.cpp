#include <unity.h>

#include "detector.h"

static const DetectorParams P = {1.0f, 10.0f, 5.0f, 3000}; // alpha 1 = no smoothing, easy to reason about

void setUp() {}
void tearDown() {}

// A triangular pass peaking at peakMs, 'height' dB over a -90 floor.
static bool runPass(Detector &d, uint32_t startMs, uint32_t peakMs, float height, uint32_t &passMs, uint32_t &lapMs)
{
    bool fired = false;
    for (uint32_t t = startMs; t < startMs + 2 * (peakMs - startMs) + 50; t += 5)
    {
        float dist = (float)(t > peakMs ? t - peakMs : peakMs - t);
        float db = -90.0f + height * (1.0f - dist / (float)(peakMs - startMs));
        if (db < -90.0f) db = -90.0f;
        if (d.feed(db, t, passMs, lapMs)) fired = true;
    }
    return fired;
}

void test_first_pass_starts_lap()
{
    Detector d(P);
    d.setBaseline(-90);
    uint32_t pass, lap;
    TEST_ASSERT_TRUE(runPass(d, 1000, 1200, 20, pass, lap));
    TEST_ASSERT_EQUAL_UINT32(1200, pass);
    TEST_ASSERT_EQUAL_UINT32(0, lap);
}

void test_second_pass_reports_lap_time()
{
    Detector d(P);
    d.setBaseline(-90);
    uint32_t pass, lap;
    runPass(d, 1000, 1200, 20, pass, lap);
    TEST_ASSERT_TRUE(runPass(d, 13000, 13200, 20, pass, lap));
    TEST_ASSERT_EQUAL_UINT32(12000, lap);
}

void test_weak_peak_ignored()
{
    Detector d(P);
    d.setBaseline(-90);
    uint32_t pass, lap;
    TEST_ASSERT_FALSE(runPass(d, 1000, 1200, 8, pass, lap));
}

void test_too_soon_rejected()
{
    Detector d(P);
    d.setBaseline(-90);
    uint32_t pass, lap;
    runPass(d, 1000, 1200, 20, pass, lap);
    TEST_ASSERT_FALSE(runPass(d, 2000, 2200, 20, pass, lap)); // 1 s later < minLapMs
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_first_pass_starts_lap);
    RUN_TEST(test_second_pass_reports_lap_time);
    RUN_TEST(test_weak_peak_ignored);
    RUN_TEST(test_too_soon_rejected);
    return UNITY_END();
}
