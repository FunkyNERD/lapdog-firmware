#include <unity.h>

#include "detector.h"

// alpha 1 = no smoothing, easy to reason about
static const DetectorParams P = {1.0f, 10.0f, 5.0f, 3000, 1500};

void setUp() {}
void tearDown() {}

// A triangular pass peaking at peakMs, 'height' dB over a -90 floor. Returns
// the last non-NONE result (NONE if nothing completed).
static PassResult runPass(Detector &d, uint32_t startMs, uint32_t peakMs, float height, Pass &pass)
{
    PassResult result = PassResult::NONE;
    for (uint32_t t = startMs; t < startMs + 2 * (peakMs - startMs) + 50; t += 5)
    {
        float dist = (float)(t > peakMs ? t - peakMs : peakMs - t);
        float db = -90.0f + height * (1.0f - dist / (float)(peakMs - startMs));
        if (db < -90.0f) db = -90.0f;
        PassResult r = d.feed(db, t, pass);
        if (r != PassResult::NONE) result = r;
    }
    return result;
}

// A quad sitting at the gate: signal high from startMs for holdMs, then gone.
static PassResult runParked(Detector &d, uint32_t startMs, uint32_t holdMs, Pass &pass)
{
    PassResult result = PassResult::NONE;
    for (uint32_t t = startMs; t < startMs + holdMs + 50; t += 5)
    {
        PassResult r = d.feed(t < startMs + holdMs ? -70.0f : -90.0f, t, pass);
        if (r != PassResult::NONE) result = r;
    }
    return result;
}

static Detector make(const DetectorParams &p = P)
{
    Detector d(p);
    d.setBaseline(-90);
    return d;
}

void test_first_pass_starts_lap()
{
    Detector d = make();
    Pass pass;
    TEST_ASSERT_EQUAL(PassResult::PASS, runPass(d, 1000, 1200, 20, pass));
    TEST_ASSERT_EQUAL_UINT32(1200, pass.atMs);
    TEST_ASSERT_EQUAL_UINT32(0, pass.lapMs);
}

void test_second_pass_reports_lap_time()
{
    Detector d = make();
    Pass pass;
    runPass(d, 1000, 1200, 20, pass);
    TEST_ASSERT_EQUAL(PassResult::PASS, runPass(d, 13000, 13200, 20, pass));
    TEST_ASSERT_EQUAL_UINT32(12000, pass.lapMs);
}

void test_weak_peak_ignored()
{
    Detector d = make();
    Pass pass;
    TEST_ASSERT_EQUAL(PassResult::NONE, runPass(d, 1000, 1200, 8, pass));
}

void test_too_soon_reported()
{
    Detector d = make();
    Pass pass;
    runPass(d, 1000, 1200, 20, pass);
    TEST_ASSERT_EQUAL(PassResult::TOO_SOON, runPass(d, 2000, 2200, 20, pass)); // 1 s later < minLapMs
    TEST_ASSERT_EQUAL_UINT32(1000, pass.lapMs);
    // The rejected peak doesn't move the lap reference.
    TEST_ASSERT_EQUAL(PassResult::PASS, runPass(d, 13000, 13200, 20, pass));
    TEST_ASSERT_EQUAL_UINT32(12000, pass.lapMs);
}

void test_parked_quad_rejected()
{
    Detector d = make();
    Pass pass;
    runPass(d, 1000, 1200, 20, pass);
    // Crashes into the gate 20 s later and sits there for 8 s.
    TEST_ASSERT_EQUAL(PassResult::TOO_LONG, runParked(d, 21000, 8000, pass));
    TEST_ASSERT_TRUE(pass.durationMs > 1500);
}

void test_pass_right_after_parked_quad_counts()
{
    Detector d = make();
    Pass pass;
    runPass(d, 1000, 1200, 20, pass);
    runParked(d, 21000, 8000, pass); // picked up at 29 s
    TEST_ASSERT_EQUAL(PassResult::PASS, runPass(d, 30000, 30200, 20, pass));
    TEST_ASSERT_EQUAL_UINT32(29000, pass.lapMs); // from the last *counted* pass
}

void test_parked_check_can_be_disabled()
{
    DetectorParams p = P;
    p.maxPeakMs = 0;
    Detector d = make(p);
    Pass pass;
    runPass(d, 1000, 1200, 20, pass);
    TEST_ASSERT_EQUAL(PassResult::PASS, runParked(d, 21000, 8000, pass));
}

void test_normal_pass_is_not_too_long()
{
    Detector d = make();
    Pass pass;
    // A slow 1.2 s flyby is still a pass.
    TEST_ASSERT_EQUAL(PassResult::PASS, runPass(d, 1000, 1600, 20, pass));
    TEST_ASSERT_TRUE(pass.durationMs <= 1500);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_first_pass_starts_lap);
    RUN_TEST(test_second_pass_reports_lap_time);
    RUN_TEST(test_weak_peak_ignored);
    RUN_TEST(test_too_soon_reported);
    RUN_TEST(test_parked_quad_rejected);
    RUN_TEST(test_pass_right_after_parked_quad_counts);
    RUN_TEST(test_parked_check_can_be_disabled);
    RUN_TEST(test_normal_pass_is_not_too_long);
    return UNITY_END();
}
