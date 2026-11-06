#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "calibrate.h"
#include "detector.h"

void setUp() {}
void tearDown() {}

// --- Session 1 (test/data/session1.txt), see the file's header ----------------

struct Sample
{
    uint32_t ms;
    float db;
    bool clipped;
};
static std::map<std::string, std::vector<Sample>> session;

static bool loadSession()
{
    // pio test runs from the project root; fall back to this file's directory.
    const char *paths[] = {"test/data/session1.txt", "../data/session1.txt"};
    FILE *f = nullptr;
    for (const char *p : paths)
        if ((f = fopen(p, "r"))) break;
    if (!f)
    {
        std::string here = __FILE__;
        here = here.substr(0, here.find_last_of('/')) + "/../data/session1.txt";
        f = fopen(here.c_str(), "r");
    }
    if (!f) return false;
    char line[128];
    std::vector<Sample> *seg = nullptr;
    uint32_t ms = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#') continue;
        if (line[0] == '@')
        {
            line[strcspn(line, "\r\n")] = 0;
            seg = &session[line + 1];
            continue;
        }
        unsigned dt;
        int db10, clip;
        if (!seg || sscanf(line, "%u,%d,%d", &dt, &db10, &clip) != 3) continue;
        ms += dt;
        seg->push_back({ms, db10 / 10.0f, clip != 0});
    }
    fclose(f);
    return true;
}

static calib::Bucket buckets[4000];

static calib::Flight analyseSegment(const char *name, calib::Recorder &rec)
{
    rec.reset();
    for (const Sample &s : session[name]) rec.add(s.ms, s.db, s.clipped);
    return calib::analyse(rec);
}

static float floorOf(const char *name)
{
    calib::Recorder rec(buckets, 4000);
    for (const Sample &s : session[name]) rec.add(s.ms, s.db, s.clipped);
    return rec.meanDb();
}

struct Replay
{
    unsigned passes = 0, tooSoon = 0, tooLong = 0;
    std::vector<uint32_t> laps; // ms, between counted passes
};

static Replay replay(const char *name, Detector &d)
{
    Replay r;
    Pass p;
    for (const Sample &s : session[name])
    {
        switch (d.feed(s.db, s.ms, p))
        {
        case PassResult::PASS:
            if (r.passes++) r.laps.push_back(p.lapMs);
            break;
        case PassResult::TOO_SOON: r.tooSoon++; break;
        case PassResult::TOO_LONG: r.tooLong++; break;
        case PassResult::NONE: break;
        }
    }
    return r;
}

// Jamie's settings for that session, apart from the thresholds.
static DetectorParams params(float enter, float exit)
{
    return {0.2f, enter, exit, 5000, 1500};
}

// --- Recorder and analyse() on synthetic data -----------------------------------

void test_median_of_envelope()
{
    calib::Recorder rec(buckets, 4000);
    // 10 buckets at -40, 5 at -20: median -40 (bucket maxima, not samples).
    uint32_t t = 0;
    for (int b = 0; b < 15; b++)
        for (int k = 0; k < 10; k++, t += 10) rec.add(t, b < 10 ? -40.0f : -20.0f, false);
    TEST_ASSERT_EQUAL(15, rec.size());
    TEST_ASSERT_FLOAT_WITHIN(0.25f, -40.0f, rec.medianDb());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, (100 * -40.0f + 50 * -20.0f) / 150, rec.meanDb());
}

void test_recorder_stops_when_full()
{
    calib::Bucket small[3];
    calib::Recorder rec(small, 3);
    for (uint32_t t = 0; t < 1000; t += 10) rec.add(t, -30.0f, false);
    TEST_ASSERT_EQUAL(3, rec.size());
    TEST_ASSERT_FLOAT_WITHIN(0.25f, -30.0f, rec.medianDb());
}

// A flight: -40 dBFS, with a 300 ms bump to peakDb every lapMs.
static void fly(calib::Recorder &rec, unsigned laps, uint32_t lapMs, float peakDb, bool clip = false)
{
    rec.reset();
    for (uint32_t t = 0; t < laps * lapMs + lapMs / 2; t += 8)
    {
        uint32_t inLap = t % lapMs;
        bool atGate = t >= lapMs / 2 && inLap < 300;
        rec.add(t, atGate ? peakDb : -40.0f, atGate && clip);
    }
}

void test_finds_each_pass()
{
    calib::Recorder rec(buckets, 4000);
    fly(rec, 6, 8000, -12.0f);
    calib::Flight f = calib::analyse(rec);
    TEST_ASSERT_EQUAL(6, f.passes);
    TEST_ASSERT_EQUAL(0, f.clippedPasses);
    TEST_ASSERT_FLOAT_WITHIN(0.25f, -40.0f, f.inFlightDb);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -12.0f, f.passDb);
}

void test_small_bumps_are_not_passes()
{
    calib::Recorder rec(buckets, 4000);
    fly(rec, 6, 8000, -30.0f); // only 10 dB over the in-flight level
    TEST_ASSERT_EQUAL(0, calib::analyse(rec).passes);
}

// --- advise() ---------------------------------------------------------------------

static calib::Flight flight(unsigned passes, unsigned clipped, float inFlight, float pass)
{
    return {passes, clipped, inFlight, pass, pass - 3, pass + 3};
}

void test_thresholds_between_flight_and_pass()
{
    calib::Advice a = calib::advise(flight(8, 0, -40, -15), -52, 20, 83);
    TEST_ASSERT_EQUAL(calib::Verdict::OK, a.verdict);
    TEST_ASSERT_EQUAL(20, a.gain);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, a.contrastDb);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12 + 0.6f * 25, a.enterDb);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12 + 0.35f * 25, a.exitDb);
    TEST_ASSERT_FALSE(a.lowContrast);
}

void test_clipped_passes_lower_the_gain()
{
    calib::Advice a = calib::advise(flight(8, 3, -30, -3), -53, 40, 83);
    TEST_ASSERT_EQUAL(calib::Verdict::LOWER_GAIN, a.verdict);
    TEST_ASSERT_EQUAL(32, a.gain);
}

void test_too_little_headroom_lowers_the_gain()
{
    calib::Advice a = calib::advise(flight(8, 0, -30, -4), -53, 40, 83); // strongest -1 dBFS
    TEST_ASSERT_EQUAL(calib::Verdict::LOWER_GAIN, a.verdict);
}

void test_weak_passes_raise_the_gain_but_not_into_clipping()
{
    calib::Advice a = calib::advise(flight(8, 0, -50, -32), -53, 16, 83);
    TEST_ASSERT_EQUAL(calib::Verdict::RAISE_GAIN, a.verdict);
    TEST_ASSERT_EQUAL(24, a.gain);
    a = calib::advise(flight(8, 0, -50, -32), -53, 16, 83, 20); // 20 clipped before
    TEST_ASSERT_EQUAL(calib::Verdict::RAISE_GAIN, a.verdict);
    TEST_ASSERT_EQUAL(19, a.gain);
    a = calib::advise(flight(8, 0, -50, -32), -53, 16, 83, 17);
    TEST_ASSERT_EQUAL(calib::Verdict::OK, a.verdict);
    TEST_ASSERT_EQUAL(16, a.gain);
}

void test_too_few_passes()
{
    calib::Advice a = calib::advise(flight(2, 0, -40, -15), -52, 20, 83);
    TEST_ASSERT_EQUAL(calib::Verdict::TOO_FEW_PASSES, a.verdict);
}

void test_low_contrast_is_flagged()
{
    calib::Advice a = calib::advise(flight(8, 0, -40, -30), -52, 20, 83);
    TEST_ASSERT_TRUE(a.lowContrast);
}

// --- Detector: a fixed (calibrated) baseline --------------------------------------

void test_fixed_baseline_survives_reset()
{
    Detector d(params(20, 10));
    d.setBaseline(-50);
    d.reset();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -50.0f, d.baseline());
    Pass p;
    for (uint32_t t = 0; t < 100; t += 5) d.feed(-45, t, p); // +5: nothing
    TEST_ASSERT_FALSE(d.inPeak());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -50.0f, d.baseline());
}

// --- Session 1 replay ---------------------------------------------------------------

void test_session_floor()
{
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -53.0f, floorOf("off40"));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -52.3f, floorOf("floor20"));
}

void test_session_gain40_clips_and_asks_for_less()
{
    calib::Recorder rec(buckets, 4000);
    calib::Flight f = analyseSegment("fly40", rec);
    TEST_ASSERT_GREATER_OR_EQUAL(8, f.passes);
    TEST_ASSERT_GREATER_THAN(f.passes / 2, f.clippedPasses);
    calib::Advice a = calib::advise(f, floorOf("off40"), 40, 83);
    TEST_ASSERT_EQUAL(calib::Verdict::LOWER_GAIN, a.verdict);
    TEST_ASSERT_EQUAL(32, a.gain);
}

void test_session_gain20_calibrates()
{
    calib::Recorder rec(buckets, 4000);
    calib::Flight f = analyseSegment("fly20", rec);
    calib::Advice a = calib::advise(f, floorOf("floor20"), 20, 83);
    char msg[160];
    snprintf(msg, sizeof(msg), "passes %u, flight %.1f, pass %.1f, enter %.1f, exit %.1f", f.passes, f.inFlightDb,
             f.passDb, a.enterDb, a.exitDb);
    TEST_MESSAGE(msg);
    TEST_ASSERT_EQUAL_MESSAGE(calib::Verdict::OK, a.verdict, msg);
    TEST_ASSERT_EQUAL_MESSAGE(0, f.clippedPasses, msg);
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(8, f.passes, msg);
    TEST_ASSERT_FALSE_MESSAGE(a.lowContrast, msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(4.0f, 28.0f, a.enterDb, msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(4.0f, 21.0f, a.exitDb, msg);
}

// The calibrated detector times the gain-20 flight: lap after lap, every lap
// a believable time (crashes made a couple slow), at most one parked reject,
// and "too soon" only for the couple of gate hits (with merging; without it a
// pass split in two showed up as ~10 of them).
void test_session_gain20_times_the_laps()
{
    calib::Recorder rec(buckets, 4000);
    float floor = floorOf("floor20");
    calib::Advice a = calib::advise(analyseSegment("fly20", rec), floor, 20, 83);
    Detector d(params(a.enterDb, a.exitDb));
    d.setBaseline(floor);
    d.mergeWithin(calib::MERGE_MS);
    Replay r = replay("fly20", d);
    char msg[200];
    int n = snprintf(msg, sizeof(msg), "passes %u, too soon %u, too long %u, laps:", r.passes, r.tooSoon, r.tooLong);
    for (uint32_t l : r.laps) n += snprintf(msg + n, sizeof(msg) - n, " %.1f", l / 1000.0f);
    TEST_MESSAGE(msg);
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(8, r.passes, msg);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(1, r.tooLong, msg);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(3, r.tooSoon, msg);
    for (uint32_t l : r.laps)
    {
        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(5000, l, msg);
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(20000, l, msg);
    }
}

// What went wrong: the tracked baseline with 6/3 dB turns laps into "too long".
void test_session_old_settings_reject_the_laps()
{
    Detector d(params(6, 3));
    d.trackBaseline(0.002f);
    Replay r = replay("fly20", d);
    TEST_ASSERT_GREATER_OR_EQUAL(5, r.tooLong);
    TEST_ASSERT_LESS_THAN(r.tooLong, r.passes);
}

// Quad off: nothing at all with the calibrated detector.
void test_session_quad_off_is_quiet()
{
    Detector d(params(28, 21));
    d.setBaseline(floorOf("off40"));
    Replay r = replay("off40", d);
    TEST_ASSERT_EQUAL(0, r.passes + r.tooSoon + r.tooLong);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_median_of_envelope);
    RUN_TEST(test_recorder_stops_when_full);
    RUN_TEST(test_finds_each_pass);
    RUN_TEST(test_small_bumps_are_not_passes);
    RUN_TEST(test_thresholds_between_flight_and_pass);
    RUN_TEST(test_clipped_passes_lower_the_gain);
    RUN_TEST(test_too_little_headroom_lowers_the_gain);
    RUN_TEST(test_weak_passes_raise_the_gain_but_not_into_clipping);
    RUN_TEST(test_too_few_passes);
    RUN_TEST(test_low_contrast_is_flagged);
    RUN_TEST(test_fixed_baseline_survives_reset);
    if (!loadSession())
    {
        printf("test/data/session1.txt not found\n");
        return UNITY_END() + 1;
    }
    RUN_TEST(test_session_floor);
    RUN_TEST(test_session_gain40_clips_and_asks_for_less);
    RUN_TEST(test_session_gain20_calibrates);
    RUN_TEST(test_session_gain20_times_the_laps);
    RUN_TEST(test_session_old_settings_reject_the_laps);
    RUN_TEST(test_session_quad_off_is_quiet);
    return UNITY_END();
}
