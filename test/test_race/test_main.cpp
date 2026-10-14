#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "race.h"

using namespace race;

void setUp() {}
void tearDown() {}

// Records what a session says, the way the device sink would route it.
struct Recorder : Sink
{
    std::vector<lapframe::Frame> frames;
    std::vector<std::string> osdRows; // "row:text"
    int clears = 0;
    int bursts = 0;

    void frame(const lapframe::Frame &f) override { frames.push_back(f); }
    void osd(uint8_t row, const char *text) override { osdRows.push_back(std::to_string(row) + ":" + text); }
    void osdClear() override { clears++; }
    void flush() override { bursts++; }

    std::vector<lapframe::Frame> of(lapframe::Type t) const
    {
        std::vector<lapframe::Frame> out;
        for (const auto &f : frames)
            if (f.type == t) out.push_back(f);
        return out;
    }
    void clear()
    {
        frames.clear();
        osdRows.clear();
        bursts = 0;
    }
};

static Setup setup(Mode mode, Limit limit = LIMIT_LAPS, uint8_t laps = 3, uint16_t timeS = 120)
{
    Setup s = {};
    s.mode = mode;
    s.limit = limit;
    s.laps = laps;
    s.timeS = timeS;
    s.heat = 4;
    s.event = "Thursday Backyard";
    s.heatPrefix = "Heat";
    s.msg = defaultMessages();
    return s;
}

void test_format()
{
    char b[32];
    formatTime(14630, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("0:14.630", b);
    formatTime(125004, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2:05.004", b);
    lapText(1, 14630, true, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("L1:0:14.630 | BEST", b);
    lapText(12, 15034, false, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("L12:0:15.034", b);
}

void test_arm_sends_session_and_osd_rows()
{
    Recorder r;
    Session s(r);
    s.arm(setup(PRACTICE), 0);
    TEST_ASSERT_EQUAL(PRACTICE_RUN, s.stage());
    TEST_ASSERT_EQUAL(1, r.bursts);
    TEST_ASSERT_EQUAL(1, r.clears);
    auto armed = r.of(lapframe::ARMED);
    TEST_ASSERT_EQUAL(1, armed.size());
    TEST_ASSERT_EQUAL(4, armed[0].heat);
    TEST_ASSERT_EQUAL(PRACTICE, armed[0].mode);
    TEST_ASSERT_EQUAL(2, r.osdRows.size());
    TEST_ASSERT_EQUAL_STRING("0:THURSDAY BACKYARD", r.osdRows[0].c_str());
    TEST_ASSERT_EQUAL_STRING("2:HEAT 4", r.osdRows[1].c_str());
    // Event name and prefix as typed, for the app's headline.
    bool sawEvent = false;
    for (const auto &f : r.of(lapframe::TEXT))
        if (f.slot == lapframe::SLOT_EVENT)
        {
            sawEvent = true;
            TEST_ASSERT_EQUAL_STRING("Thursday Backyard", f.text);
        }
    TEST_ASSERT_TRUE(sawEvent);
    // Seqs are unique within a burst (the app de-dupes by seq).
    for (size_t i = 1; i < r.frames.size(); i++) TEST_ASSERT_EQUAL(r.frames[i - 1].seq + 1, r.frames[i].seq);
}

void test_toggles_silence_rows()
{
    Recorder r;
    Session s(r);
    Setup st = setup(PRACTICE);
    st.msg.event = false;
    st.msg.heat = false;
    st.msg.laps = false;
    s.arm(st, 0);
    TEST_ASSERT_EQUAL(0, r.osdRows.size());
    s.pass(1000);
    s.pass(16000);
    TEST_ASSERT_EQUAL(0, r.osdRows.size());
    TEST_ASSERT_EQUAL(2, r.of(lapframe::LAP).size()); // the app still gets laps
}

void test_practice_first_pass_starts_lap_1()
{
    Recorder r;
    Session s(r);
    s.arm(setup(PRACTICE), 0);
    r.clear();
    s.pass(1000);
    auto laps = r.of(lapframe::LAP);
    TEST_ASSERT_EQUAL(1, laps.size());
    TEST_ASSERT_EQUAL(0, laps[0].lap);
    TEST_ASSERT_EQUAL(0, s.lapsDone());
    TEST_ASSERT_EQUAL(0, r.osdRows.size());

    s.pass(15630);
    s.pass(30000);
    laps = r.of(lapframe::LAP);
    TEST_ASSERT_EQUAL(3, laps.size());
    TEST_ASSERT_EQUAL(1, laps[1].lap);
    TEST_ASSERT_EQUAL_UINT32(14630, laps[1].ms);
    TEST_ASSERT_EQUAL(lapframe::LAP_BEST, laps[1].flags);
    TEST_ASSERT_EQUAL_UINT32(14370, laps[2].ms);
    TEST_ASSERT_EQUAL(lapframe::LAP_BEST, laps[2].flags);
    TEST_ASSERT_EQUAL_STRING("15:L1:0:14.630 | BEST", r.osdRows[0].c_str());

    s.pass(45000); // slower: no BEST
    laps = r.of(lapframe::LAP);
    TEST_ASSERT_EQUAL(0, laps[3].flags);
    TEST_ASSERT_EQUAL_STRING("15:L3:0:15.000", r.osdRows.back().c_str());
    TEST_ASSERT_EQUAL(3, s.lapsDone());
    TEST_ASSERT_EQUAL_UINT32(14370, s.bestMs());
}

void test_best_callout_toggle()
{
    Recorder r;
    Session s(r);
    Setup st = setup(PRACTICE);
    st.msg.best = false;
    s.arm(st, 0);
    s.pass(0);
    s.pass(14630);
    TEST_ASSERT_EQUAL_STRING("15:L1:0:14.630", r.osdRows.back().c_str());
    TEST_ASSERT_EQUAL(lapframe::LAP_BEST, r.of(lapframe::LAP).back().flags);
}

void test_race_stage_hold_go()
{
    Recorder r;
    Session s(r);
    s.arm(setup(RACE), 2000);
    TEST_ASSERT_EQUAL(STAGING, s.stage());
    TEST_ASSERT_EQUAL_STRING("5:ARM NOW", r.osdRows.back().c_str());
    auto armed = r.of(lapframe::ARMED);
    TEST_ASSERT_EQUAL(3, armed[0].lapLimit);
    TEST_ASSERT_EQUAL(0, armed[0].timeLimitS);
    TEST_ASSERT_EQUAL(STAGING, r.of(lapframe::RACE).back().stage);

    s.pass(500); // sitting at the gate before GO
    TEST_ASSERT_EQUAL(0, r.of(lapframe::LAP).size());

    s.tick(1000); // hold starts counting here
    s.tick(2999);
    TEST_ASSERT_EQUAL(STAGING, s.stage());
    s.tick(3000);
    TEST_ASSERT_EQUAL(RACING, s.stage());
    TEST_ASSERT_EQUAL_STRING("5:GO!", r.osdRows.back().c_str());
    TEST_ASSERT_EQUAL(RACING, r.of(lapframe::RACE).back().stage);
}

void test_race_lap_limit_finishes()
{
    Recorder r;
    Session s(r);
    s.arm(setup(RACE, LIMIT_LAPS, 2), 1000);
    s.tick(0);
    s.tick(1000); // GO at 1000
    s.pass(5671); // holeshot
    auto laps = r.of(lapframe::LAP);
    TEST_ASSERT_EQUAL(0, laps.back().lap);
    TEST_ASSERT_EQUAL_UINT32(4671, laps.back().ms);

    s.pass(20301);
    TEST_ASSERT_EQUAL(RACING, s.stage());
    s.pass(35000);
    TEST_ASSERT_EQUAL(FINISHED, s.stage());
    laps = r.of(lapframe::LAP);
    TEST_ASSERT_EQUAL(2, laps.back().lap);
    TEST_ASSERT_TRUE(laps.back().flags & lapframe::LAP_FINAL);
    TEST_ASSERT_EQUAL_STRING("5:FINISHED!", r.osdRows.back().c_str());
    auto race = r.of(lapframe::RACE).back();
    TEST_ASSERT_EQUAL(FINISHED, race.stage);
    TEST_ASSERT_EQUAL_UINT32(34000, race.ms);

    size_t n = r.frames.size();
    s.pass(50000); // after the finish: ignored
    TEST_ASSERT_EQUAL(n, r.frames.size());
}

void test_race_time_limit_finishes_on_next_pass()
{
    Recorder r;
    Session s(r);
    s.arm(setup(RACE, LIMIT_TIME, 0, 30), 0);
    s.tick(0);
    s.tick(0); // GO at 0
    TEST_ASSERT_EQUAL(30, r.of(lapframe::ARMED)[0].timeLimitS);
    s.pass(4000);
    s.pass(18000);
    s.tick(30000); // time's up, but the lap in progress counts
    TEST_ASSERT_EQUAL(RACING, s.stage());
    s.pass(33000);
    TEST_ASSERT_EQUAL(FINISHED, s.stage());
    TEST_ASSERT_EQUAL(2, s.lapsDone());
}

void test_disarm()
{
    Recorder r;
    Session s(r);
    s.arm(setup(PRACTICE), 0);
    s.pass(0);
    s.pass(15000);
    r.clear();
    s.disarm();
    TEST_ASSERT_FALSE(s.armed());
    auto d = r.of(lapframe::DISARMED);
    TEST_ASSERT_EQUAL(1, d.size());
    TEST_ASSERT_EQUAL(4, d[0].heat);
    TEST_ASSERT_EQUAL(1, d[0].lapLimit);
    s.disarm(); // already idle: nothing
    TEST_ASSERT_EQUAL(1, r.of(lapframe::DISARMED).size());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_format);
    RUN_TEST(test_arm_sends_session_and_osd_rows);
    RUN_TEST(test_toggles_silence_rows);
    RUN_TEST(test_practice_first_pass_starts_lap_1);
    RUN_TEST(test_best_callout_toggle);
    RUN_TEST(test_race_stage_hold_go);
    RUN_TEST(test_race_lap_limit_finishes);
    RUN_TEST(test_race_time_limit_finishes_on_next_pass);
    RUN_TEST(test_disarm);
    return UNITY_END();
}
