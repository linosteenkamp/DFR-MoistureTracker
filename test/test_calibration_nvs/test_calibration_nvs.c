#include <unity.h>
#include "nvs_flash.h"
#include "soil_calibration.h"

void setUp(void) {
    // Fresh NVS partition for every test
    nvs_flash_erase();
    nvs_flash_init();
    soil_calibration_clear();
    soil_calibration_init();
}

void tearDown(void) {
    soil_calibration_clear();
}

static void test_defaults_on_empty_nvs(void) {
    TEST_ASSERT_EQUAL_UINT32(2800, soil_calibration_get_dry_mv());
    TEST_ASSERT_EQUAL_UINT32(0,    soil_calibration_get_wet_mv());
}

static void test_round_trip_persists_through_reinit(void) {
    TEST_ASSERT_TRUE(soil_calibration_save(2700, 600, 42));
    // simulate reboot
    soil_calibration_init();
    TEST_ASSERT_EQUAL_UINT32(2700, soil_calibration_get_dry_mv());
    TEST_ASSERT_EQUAL_UINT32(600,  soil_calibration_get_wet_mv());
    TEST_ASSERT_EQUAL_UINT32(42,   soil_calibration_get_cal_ts());
}

static void test_clear_resets_to_defaults(void) {
    soil_calibration_save(2700, 600, 42);
    TEST_ASSERT_TRUE(soil_calibration_clear());
    soil_calibration_init();
    TEST_ASSERT_EQUAL_UINT32(2800, soil_calibration_get_dry_mv());
}

void app_main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_on_empty_nvs);
    RUN_TEST(test_round_trip_persists_through_reinit);
    RUN_TEST(test_clear_resets_to_defaults);
    UNITY_END();
}
