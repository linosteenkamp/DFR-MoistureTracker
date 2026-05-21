#include <unity.h>

// Pull the pure function in directly so we don't need ESP-IDF.
// The function is defined in src/soil_moisture.c but guarded so its
// ESP-IDF-dependent code is excluded under TEST_HOST.
#define TEST_HOST 1
#include "../../src/soil_moisture.c"

void setUp(void) {}
void tearDown(void) {}

static void test_at_dry_returns_zero(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, soil_moisture_calc_percentage(2800, 2800, 0));
}

static void test_at_wet_returns_one_hundred(void) {
    TEST_ASSERT_EQUAL_FLOAT(100.0f, soil_moisture_calc_percentage(0, 2800, 0));
}

static void test_midpoint_returns_fifty(void) {
    TEST_ASSERT_EQUAL_FLOAT(50.0f, soil_moisture_calc_percentage(1400, 2800, 0));
}

static void test_above_dry_clamps_to_zero(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, soil_moisture_calc_percentage(3500, 2800, 0));
}

static void test_below_wet_clamps_to_hundred(void) {
    TEST_ASSERT_EQUAL_FLOAT(100.0f, soil_moisture_calc_percentage(-100, 2800, 0));
}

static void test_handles_nonzero_wet_baseline(void) {
    // dry=2950, wet=850, reading=1900 → (2950-1900)/(2950-850) = 50%
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, soil_moisture_calc_percentage(1900, 2950, 850));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_at_dry_returns_zero);
    RUN_TEST(test_at_wet_returns_one_hundred);
    RUN_TEST(test_midpoint_returns_fifty);
    RUN_TEST(test_above_dry_clamps_to_zero);
    RUN_TEST(test_below_wet_clamps_to_hundred);
    RUN_TEST(test_handles_nonzero_wet_baseline);
    return UNITY_END();
}
