#include <unity.h>
#include "../../src/ias_status.c"

void setUp(void) {}
void tearDown(void) {}

static void test_open_sets_alarm1(void)     { TEST_ASSERT_EQUAL_UINT16(0x0001, ias_zone_status_encode(true)); }
static void test_closed_clears_status(void) { TEST_ASSERT_EQUAL_UINT16(0x0000, ias_zone_status_encode(false)); }
static void test_alarm1_constant(void)      { TEST_ASSERT_EQUAL_UINT16(0x0001, IAS_ZONE_STATUS_ALARM1); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_sets_alarm1);
    RUN_TEST(test_closed_clears_status);
    RUN_TEST(test_alarm1_constant);
    return UNITY_END();
}
