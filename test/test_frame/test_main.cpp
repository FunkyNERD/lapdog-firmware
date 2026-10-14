#include <unity.h>

#include "lap_frame.h"

void setUp() {}
void tearDown() {}

void test_roundtrip()
{
    lapframe::Frame in = {lapframe::LAP, 42, 3, 12345};
    uint8_t b[lapframe::SIZE];
    lapframe::encode(in, b);
    // Pinned bytes - the Android parser's test uses the same vector.
    const uint8_t expected[] = {0x11, 0x2A, 0x03, 0x39, 0x30, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, b, sizeof(expected));

    lapframe::Frame out;
    TEST_ASSERT_TRUE(lapframe::decode(b, sizeof(b), out));
    TEST_ASSERT_EQUAL(lapframe::LAP, out.type);
    TEST_ASSERT_EQUAL(42, out.seq);
    TEST_ASSERT_EQUAL(3, out.pilot);
    TEST_ASSERT_EQUAL_UINT32(12345, out.value);
}

void test_rejects_wrong_version()
{
    uint8_t b[lapframe::SIZE] = {0x21, 0, 0, 0, 0, 0, 0};
    lapframe::Frame out;
    TEST_ASSERT_FALSE(lapframe::decode(b, sizeof(b), out));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_rejects_wrong_version);
    return UNITY_END();
}
