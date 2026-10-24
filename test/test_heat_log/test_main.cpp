#include <string.h>
#include <unity.h>

#include <string>

#include "heat_log.h"
#include "race.h"

using namespace race;

void setUp() {}
void tearDown() {}

// The session's frames straight into a Recorder, as the timer wires it.
struct Tap : Sink
{
    heatlog::Recorder rec;
    int finished = 0;
    heatlog::Heat last = {};
    void frame(const lapframe::Frame &f) override
    {
        if (rec.frame(f))
        {
            finished++;
            last = rec.done();
        }
    }
    void osd(uint8_t, const char *) override {}
    void osdClear() override {}
    void flush() override {}
};

static Setup setup(Mode mode, uint8_t laps = 3)
{
    Setup s = {};
    s.mode = mode;
    s.limit = LIMIT_LAPS;
    s.laps = laps;
    s.heat = 4;
    s.event = "Thursday Backyard";
    s.heatPrefix = "Heat";
    s.msg = defaultMessages();
    return s;
}

static std::string json(const heatlog::Heat &h, int64_t at = 0)
{
    static char buf[4096];
    size_t n = heatlog::uploadJson(h, at, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    return std::string(buf, n);
}

void test_practice_heat_is_recorded_on_disarm()
{
    Tap t;
    Session s(t);
    t.rec.context("Thursday Backyard", "Heat", 1760241290, 7, 1000);
    s.arm(setup(PRACTICE), 0);
    s.tick(1000);
    s.pass(2000);  // start pass
    s.pass(23345); // lap 1: 21.345
    s.pass(44216); // lap 2: 20.871
    s.disarm(50000);
    TEST_ASSERT_EQUAL(1, t.finished);
    TEST_ASSERT_EQUAL(4, t.last.heat);
    TEST_ASSERT_EQUAL(2, t.last.lapCount);
    TEST_ASSERT_EQUAL_UINT32(21345, t.last.laps[0].ms);
    TEST_ASSERT_EQUAL(-1, t.last.holeshotMs);
    TEST_ASSERT_EQUAL_STRING(
        "{\"lap_count\":2,\"total_time_s\":42.216,\"best_lap_s\":20.871,\"label\":\"Heat 4 \xC2\xB7 Practice\","
        "\"event\":\"Thursday Backyard\",\"started_at\":1760241290,\"laps\":["
        "{\"lap\":1,\"t_ms\":21345,\"time_s\":21.345,\"best\":false},"
        "{\"lap\":2,\"t_ms\":42216,\"time_s\":20.871,\"best\":true}],"
        "\"source\":\"timer\",\"mode\":\"practice\",\"timer_heat\":4}",
        json(t.last, 1760241290).c_str());
}

void test_race_has_holeshot_and_race_time()
{
    Tap t;
    Session s(t);
    t.rec.context("", "Heat", 0, 7, 1000); // the clock was never set
    s.arm(setup(RACE, 2), 500);
    s.tick(1000); // staging; GO 500 ms after this
    s.tick(1500); // GO
    TEST_ASSERT_EQUAL(RACING, s.stage());
    s.pass(3000);  // holeshot 1.500
    s.pass(24345); // lap 1
    s.pass(45216); // lap 2: the limit
    s.tick(45300);
    TEST_ASSERT_EQUAL(FINISHED, s.stage());
    s.disarm(50000);
    TEST_ASSERT_EQUAL(1, t.finished);
    TEST_ASSERT_EQUAL(1500, t.last.holeshotMs);
    TEST_ASSERT_TRUE(t.last.finished);
    TEST_ASSERT_TRUE(t.last.raceMs > 0);
    std::string j = json(t.last);
    TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"label\":\"Heat 4 \xC2\xB7 Race\""));
    TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "{\"lap\":1,\"t_ms\":22845,\"time_s\":21.345,\"best\":false}"));
    TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"mode\":\"race\""));
    TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"holeshot_ms\":1500"));
    TEST_ASSERT_NULL(strstr(j.c_str(), "started_at")); // the clock was never set
}

void test_nothing_timed_is_not_a_heat()
{
    Tap t;
    Session s(t);
    s.arm(setup(PRACTICE), 0);
    s.tick(1000);
    s.disarm(2000);
    TEST_ASSERT_EQUAL(0, t.finished);
}

void test_start_pass_only_is_a_heat_but_not_uploadable()
{
    Tap t;
    Session s(t);
    s.arm(setup(PRACTICE), 0);
    s.tick(1000);
    s.pass(2000);
    s.disarm(3000);
    TEST_ASSERT_EQUAL(1, t.finished);
    TEST_ASSERT_FALSE(heatlog::uploadable(t.last));
}

void test_new_heat_while_armed_ends_the_last()
{
    Tap t;
    Session s(t);
    Setup a = setup(PRACTICE);
    s.arm(a, 0);
    s.tick(1000);
    s.pass(2000);
    s.pass(23345);
    s.disarm(24000); // "heat next" disarms, then arms the next heat
    a.heat = 5;
    s.arm(a, 0);
    TEST_ASSERT_EQUAL(1, t.finished);
    TEST_ASSERT_EQUAL(4, t.last.heat);
    TEST_ASSERT_TRUE(t.rec.active());
}

void test_ignored_passes_count()
{
    Tap t;
    Session s(t);
    s.arm(setup(PRACTICE), 0);
    s.tick(1000);
    s.pass(2000);
    s.ignored(lapframe::IGNORED_TOO_SOON, 800);
    s.ignored(lapframe::IGNORED_TOO_LONG, 2500);
    s.disarm(3000);
    TEST_ASSERT_EQUAL(1, t.last.ignoredSoon);
    TEST_ASSERT_EQUAL(1, t.last.ignoredLong);
}

void test_json_escapes_and_fits()
{
    heatlog::Heat h = {};
    h.heat = 1;
    h.holeshotMs = h.raceMs = -1;
    snprintf(h.event, sizeof(h.event), "%s", "Bob's \"yard\"");
    h.lapCount = 1;
    h.laps[0] = {1, 20000};
    std::string j = json(h);
    TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"event\":\"Bob's \\\"yard\\\"\""));
    TEST_ASSERT_NOT_NULL(strstr(j.c_str(), "\"label\":\"1 \xC2\xB7 Practice\""));
    char small[16];
    TEST_ASSERT_EQUAL(0, heatlog::uploadJson(h, 0, small, sizeof(small)));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_practice_heat_is_recorded_on_disarm);
    RUN_TEST(test_race_has_holeshot_and_race_time);
    RUN_TEST(test_nothing_timed_is_not_a_heat);
    RUN_TEST(test_start_pass_only_is_a_heat_but_not_uploadable);
    RUN_TEST(test_new_heat_while_armed_ends_the_last);
    RUN_TEST(test_ignored_passes_count);
    RUN_TEST(test_json_escapes_and_fits);
    return UNITY_END();
}
