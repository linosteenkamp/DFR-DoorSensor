#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "../../include/ota_ids.h"

void setUp(void) {}
void tearDown(void) {}

/* Mirror of the z2m frontend's fileVersion2String():
 *   s = hex(v).padStart(8,'0');
 *   `${s[0]}.${s[1]}.${s[2..4]}-${s[4]}.${s[5]}.${s[6..8]}`
 * This is exactly how z2m renders the OTA "installed"/"available" columns, so
 * the test asserts on what a human actually sees. */
static void z2m_render(uint32_t v, char *out)
{
    char s[9];
    snprintf(s, sizeof s, "%08x", v);
    snprintf(out, 16, "%c.%c.%c%c-%c.%c.%c%c",
             s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
}

/* The fix: semver must land in the high hex digits so the major shows first. */
static void test_v1_0_2_packs_to_high_bytes(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x10020000u, OTA_PACK_VERSION(1, 0, 2, 0));
}

static void test_v1_0_2_renders_with_visible_major(void)
{
    char out[16];
    z2m_render(OTA_PACK_VERSION(1, 0, 2, 0), out);
    TEST_ASSERT_EQUAL_STRING("1.0.02-0.0.00", out);
}

/* Higher semver must yield a strictly larger uint32 (z2m only offers an update
 * when the image's fileVersion exceeds the device's current one). */
static void test_monotonic_ordering(void)
{
    uint32_t v102 = OTA_PACK_VERSION(1, 0, 2, 0);
    uint32_t v103 = OTA_PACK_VERSION(1, 0, 3, 0);
    uint32_t v110 = OTA_PACK_VERSION(1, 1, 0, 0);
    uint32_t v200 = OTA_PACK_VERSION(2, 0, 0, 0);
    TEST_ASSERT_TRUE(v102 < v103);
    TEST_ASSERT_TRUE(v103 < v110);
    TEST_ASSERT_TRUE(v110 < v200);
}

/* Must stay above every legacy-scheme value (all had top byte 0x01) so devices
 * still on old firmware are offered the upgrade. */
static void test_supersedes_legacy_scheme(void)
{
    uint32_t legacy_installed = 0x01000100u; /* what the bench unit reports today */
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 0, 3, 0) > legacy_installed);
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 0, 3, 0) > 0x01010002u); /* released v1.0.2 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_v1_0_2_packs_to_high_bytes);
    RUN_TEST(test_v1_0_2_renders_with_visible_major);
    RUN_TEST(test_monotonic_ordering);
    RUN_TEST(test_supersedes_legacy_scheme);
    return UNITY_END();
}
