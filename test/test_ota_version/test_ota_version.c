#include <unity.h>
#include "ota_ids.h"

void setUp(void) {}
void tearDown(void) {}

void test_pack_version_orders_bytes_major_minor_patch(void) {
    // v1.2.3 -> 0x01020300
    TEST_ASSERT_EQUAL_HEX32(0x01020300u, OTA_PACK_VERSION(1, 2, 3, 0));
}

void test_pack_version_includes_build_byte(void) {
    TEST_ASSERT_EQUAL_HEX32(0x0102030Au, OTA_PACK_VERSION(1, 2, 3, 10));
}

void test_higher_semver_is_greater_uint32(void) {
    TEST_ASSERT_TRUE(OTA_PACK_VERSION(1, 3, 0, 0) > OTA_PACK_VERSION(1, 2, 9, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pack_version_orders_bytes_major_minor_patch);
    RUN_TEST(test_pack_version_includes_build_byte);
    RUN_TEST(test_higher_semver_is_greater_uint32);
    return UNITY_END();
}
