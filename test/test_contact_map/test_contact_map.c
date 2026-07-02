#include <unity.h>
#include "../../src/contact_map.c"

void setUp(void) {}
void tearDown(void) {}

// active_low = true: LOW level (0) == door open
static void test_active_low_low_is_open(void)   { TEST_ASSERT_TRUE(contact_is_open(0, true)); }
static void test_active_low_high_is_closed(void){ TEST_ASSERT_FALSE(contact_is_open(1, true)); }
// active_low = false: HIGH level (1) == door open
static void test_active_high_high_is_open(void) { TEST_ASSERT_TRUE(contact_is_open(1, false)); }
static void test_active_high_low_is_closed(void){ TEST_ASSERT_FALSE(contact_is_open(0, false)); }
// any nonzero level counts as HIGH
static void test_nonzero_level_is_high(void)    { TEST_ASSERT_FALSE(contact_is_open(5, true)); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_active_low_low_is_open);
    RUN_TEST(test_active_low_high_is_closed);
    RUN_TEST(test_active_high_high_is_open);
    RUN_TEST(test_active_high_low_is_closed);
    RUN_TEST(test_nonzero_level_is_high);
    return UNITY_END();
}
