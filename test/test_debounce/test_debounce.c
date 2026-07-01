#include <unity.h>
#include "../../src/debounce.c"

void setUp(void) {}
void tearDown(void) {}

static void test_init_sets_stable_level(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_EQUAL_INT(1, debounce_stable_level(&d));
}

static void test_settle_same_level_no_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_FALSE(debounce_settle(&d, 1));
    TEST_ASSERT_EQUAL_INT(1, debounce_stable_level(&d));
}

static void test_settle_different_level_reports_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_TRUE(debounce_settle(&d, 0));
    TEST_ASSERT_EQUAL_INT(0, debounce_stable_level(&d));
}

static void test_settle_change_then_same_no_second_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_TRUE(debounce_settle(&d, 0));   // 1 -> 0 : change
    TEST_ASSERT_FALSE(debounce_settle(&d, 0));  // 0 -> 0 : no change
}

static void test_bounce_back_to_original_no_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    // Chatter that settles back to the original level yields no reported change.
    TEST_ASSERT_FALSE(debounce_settle(&d, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_sets_stable_level);
    RUN_TEST(test_settle_same_level_no_change);
    RUN_TEST(test_settle_different_level_reports_change);
    RUN_TEST(test_settle_change_then_same_no_second_change);
    RUN_TEST(test_bounce_back_to_original_no_change);
    return UNITY_END();
}
