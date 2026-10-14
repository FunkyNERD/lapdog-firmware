#include <unity.h>

#include "channels.h"

void setUp() {}
void tearDown() {}

void test_freq_of()
{
    TEST_ASSERT_EQUAL(5769, channels::freqOf("R4"));
    TEST_ASSERT_EQUAL(5769, channels::freqOf("r4"));
    TEST_ASSERT_EQUAL(5362, channels::freqOf("L1"));
    TEST_ASSERT_EQUAL(5945, channels::freqOf("E8"));
    TEST_ASSERT_EQUAL(0, channels::freqOf("R9"));
    TEST_ASSERT_EQUAL(0, channels::freqOf("X1"));
    TEST_ASSERT_EQUAL(0, channels::freqOf("R10"));
    TEST_ASSERT_EQUAL(0, channels::freqOf(""));
}

void test_label_of()
{
    char l[3];
    TEST_ASSERT_TRUE(channels::labelOf(5769, l));
    TEST_ASSERT_EQUAL_STRING("R4", l);
    TEST_ASSERT_TRUE(channels::labelOf(5880, l)); // R7 and F8: table order wins
    TEST_ASSERT_EQUAL_STRING("R7", l);
    TEST_ASSERT_FALSE(channels::labelOf(5801, l));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_freq_of);
    RUN_TEST(test_label_of);
    return UNITY_END();
}
