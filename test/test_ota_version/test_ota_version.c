#include <unity.h>
#include "ota_ids.h"

void setUp(void) {}
void tearDown(void) {}

void test_pack_version_puts_semver_in_low_3_bytes(void) {
    // z2m displays the LOW 3 bytes as major.minor.patch (it drops the top byte),
    // so the semver lives there; the top byte is a fixed 0x01.
    // v1.2.3 -> 0x01 01 02 03
    TEST_ASSERT_EQUAL_HEX32(0x01010203u, OTA_PACK_VERSION(1, 2, 3, 0));
}

void test_pack_version_ignores_build(void) {
    // build no longer fits (semver moved down a byte) and is ignored.
    TEST_ASSERT_EQUAL_HEX32(OTA_PACK_VERSION(1, 2, 3, 0), OTA_PACK_VERSION(1, 2, 3, 10));
}

void test_higher_semver_is_greater_uint32(void) {
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 3, 0, 0) > OTA_PACK_VERSION(1, 2, 9, 0));
}

void test_outnumbers_legacy_scheme(void) {
    // Must exceed the legacy 0x010000xx packing so devices still on legacy
    // firmware are offered the upgrade.
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 0, 0, 0) > 0x01000100u);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pack_version_puts_semver_in_low_3_bytes);
    RUN_TEST(test_pack_version_ignores_build);
    RUN_TEST(test_higher_semver_is_greater_uint32);
    RUN_TEST(test_outnumbers_legacy_scheme);
    return UNITY_END();
}
