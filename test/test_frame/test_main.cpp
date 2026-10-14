#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "lap_frame.h"

using namespace lapframe;

void setUp() {}
void tearDown() {}

// Pinned vectors - lapdog-android's LapFrameTest uses the same hex strings,
// so a layout change on one side fails the other's tests.
static void assertHex(const char *expected, const Frame &f)
{
    uint8_t b[MAX_SIZE];
    size_t n = encode(f, b);
    char hex[2 * MAX_SIZE + 1] = "";
    for (size_t i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02X", b[i]);
    TEST_ASSERT_EQUAL_STRING(expected, hex);
}

static Frame roundtrip(const Frame &in)
{
    uint8_t b[MAX_SIZE];
    size_t n = encode(in, b);
    Frame out;
    TEST_ASSERT_TRUE(decode(b, n, out));
    return out;
}

void test_lap()
{
    Frame f = {};
    f.type = LAP;
    f.seq = 42;
    f.pilot = 3;
    f.lap = 2;
    f.flags = LAP_BEST;
    f.ms = 12345;
    assertHex("212A03020139300000", f);
    Frame out = roundtrip(f);
    TEST_ASSERT_EQUAL(LAP, out.type);
    TEST_ASSERT_EQUAL(42, out.seq);
    TEST_ASSERT_EQUAL(3, out.pilot);
    TEST_ASSERT_EQUAL(2, out.lap);
    TEST_ASSERT_EQUAL(LAP_BEST, out.flags);
    TEST_ASSERT_EQUAL_UINT32(12345, out.ms);
}

void test_armed()
{
    Frame f = {};
    f.type = ARMED;
    f.seq = 1;
    f.mode = 1;
    f.heat = 4;
    f.lapLimit = 3;
    assertHex("220100010400030000", f);
    Frame out = roundtrip(f);
    TEST_ASSERT_EQUAL(1, out.mode);
    TEST_ASSERT_EQUAL(4, out.heat);
    TEST_ASSERT_EQUAL(3, out.lapLimit);
    TEST_ASSERT_EQUAL(0, out.timeLimitS);
}

void test_disarmed_heartbeat_race()
{
    Frame d = {};
    d.type = DISARMED;
    d.seq = 10;
    d.heat = 4;
    d.lapLimit = 12;
    assertHex("230A0004000C", d);

    Frame h = {};
    h.type = HEARTBEAT;
    h.seq = 9;
    h.stage = 3;
    h.mode = 1;
    h.heat = 4;
    h.ms = 100000;
    assertHex("24090003010400A0860100", h);
    TEST_ASSERT_EQUAL_UINT32(100000, roundtrip(h).ms);

    Frame r = {};
    r.type = RACE;
    r.seq = 5;
    r.stage = 4;
    r.ms = 45000;
    assertHex("25050004C8AF0000", r);
    TEST_ASSERT_EQUAL(4, roundtrip(r).stage);
}

void test_text()
{
    Frame f = {};
    uint8_t seq = 7;
    TEST_ASSERT_EQUAL(1, textFrames(SLOT_HEAT_PREFIX, "Heat", 0, seq, &f, 1));
    TEST_ASSERT_EQUAL(8, seq);
    assertHex("260700810148656174", f);
    Frame out = roundtrip(f);
    TEST_ASSERT_EQUAL(SLOT_HEAT_PREFIX, out.slot);
    TEST_ASSERT_EQUAL_STRING("Heat", out.text);
}

void test_text_chunks_fit_one_advert()
{
    const char *event = "Thursday Backyard Night Series - Round 7"; // 40 chars
    Frame f[MAX_TEXT_PARTS];
    uint8_t seq = 0;
    size_t n = textFrames(SLOT_EVENT, event, 0, seq, f, MAX_TEXT_PARTS);
    TEST_ASSERT_EQUAL(3, n);
    char joined[64] = "";
    for (size_t i = 0; i < n; i++)
    {
        uint8_t b[MAX_SIZE];
        size_t len = encode(f[i], b);
        TEST_ASSERT_TRUE(len <= MAX_SIZE);
        Frame out;
        TEST_ASSERT_TRUE(decode(b, len, out));
        TEST_ASSERT_EQUAL(i, out.part);
        TEST_ASSERT_EQUAL(3, out.parts);
        TEST_ASSERT_EQUAL(i, out.seq);
        strcat(joined, out.text);
    }
    TEST_ASSERT_EQUAL_STRING(event, joined);
    // 3 bytes of flags + 4 of manufacturer-data header + the frame = a legacy advert.
    TEST_ASSERT_TRUE(3 + 4 + MAX_SIZE <= 31);
}

void test_empty_text_is_one_frame()
{
    Frame f[2];
    uint8_t seq = 0;
    TEST_ASSERT_EQUAL(1, textFrames(15, "", 0, seq, f, 2));
    assertHex("2600000F01", f[0]);
}

void test_rejects_bad_frames()
{
    uint8_t wrongVersion[] = {0x11, 0x2A, 0x03, 0x39, 0x30, 0x00, 0x00}; // v1 LAP
    uint8_t shortLap[] = {0x21, 0x2A, 0x03, 0x02, 0x01, 0x39};
    uint8_t unknownType[] = {0x2F, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t secondPart[] = {0x26, 0, 0, 0x80, 0x12, 'x'}; // part 1 of 2 (0-based)
    uint8_t partPastEnd[] = {0x26, 0, 0, 0x80, 0x22, 'x'}; // part 2 of 2
    Frame out;
    TEST_ASSERT_FALSE(decode(wrongVersion, sizeof(wrongVersion), out));
    TEST_ASSERT_FALSE(decode(shortLap, sizeof(shortLap), out));
    TEST_ASSERT_FALSE(decode(unknownType, sizeof(unknownType), out));
    TEST_ASSERT_TRUE(decode(secondPart, sizeof(secondPart), out));
    TEST_ASSERT_FALSE(decode(partPastEnd, sizeof(partPastEnd), out));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_lap);
    RUN_TEST(test_armed);
    RUN_TEST(test_disarmed_heartbeat_race);
    RUN_TEST(test_text);
    RUN_TEST(test_text_chunks_fit_one_advert);
    RUN_TEST(test_empty_text_is_one_frame);
    RUN_TEST(test_rejects_bad_frames);
    return UNITY_END();
}
