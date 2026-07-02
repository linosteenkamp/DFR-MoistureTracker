#include <unity.h>
#include <stdio.h>
#include "ota_ids.h"

void setUp(void) {}
void tearDown(void) {}

/* Mirror of the z2m frontend's fileVersion2String():
 *   s = hex(v).padStart(8,'0');
 *   `${s[0]}.${s[1]}.${s[2..4]}-${s[4]}.${s[5]}.${s[6..8]}`
 * This is exactly how z2m renders the OTA "installed"/"available" columns, so
 * the test asserts on what a human actually sees. */
static void z2m_render(uint32_t v, char *out) {
    char s[9];
    snprintf(s, sizeof s, "%08x", v);
    snprintf(out, 16, "%c.%c.%c%c-%c.%c.%c%c",
             s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
}

/* The fix: semver lands in the high hex digits so the major shows first. */
void test_pack_version_puts_semver_in_high_digits(void) {
    // v1.2.3 -> 0x12030000
    TEST_ASSERT_EQUAL_HEX32(0x12030000u, OTA_PACK_VERSION(1, 2, 3, 0));
}

void test_v1_0_2_renders_with_visible_major(void) {
    char out[16];
    z2m_render(OTA_PACK_VERSION(1, 0, 2, 0), out);
    TEST_ASSERT_EQUAL_STRING("1.0.02-0.0.00", out);
}

void test_pack_version_ignores_build(void) {
    TEST_ASSERT_EQUAL_HEX32(OTA_PACK_VERSION(1, 2, 3, 0), OTA_PACK_VERSION(1, 2, 3, 10));
}

void test_higher_semver_is_greater_uint32(void) {
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 3, 0, 0) > OTA_PACK_VERSION(1, 2, 9, 0));
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(2, 0, 0, 0) > OTA_PACK_VERSION(1, 9, 9, 0));
}

void test_outnumbers_legacy_scheme(void) {
    // Must exceed every legacy 0x01xxxxxx value (incl. released v1.0.2 = 0x01010002)
    // so devices still on legacy firmware are offered the upgrade.
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 0, 0, 0) > 0x01000100u);
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 0, 3, 0) > 0x01010002u);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pack_version_puts_semver_in_high_digits);
    RUN_TEST(test_v1_0_2_renders_with_visible_major);
    RUN_TEST(test_pack_version_ignores_build);
    RUN_TEST(test_higher_semver_is_greater_uint32);
    RUN_TEST(test_outnumbers_legacy_scheme);
    return UNITY_END();
}
