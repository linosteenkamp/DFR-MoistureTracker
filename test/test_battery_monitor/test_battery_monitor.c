#include <unity.h>
#include <math.h>

// Include SUT source directly under TEST_HOST so we don't have to link ESP-IDF.
#define TEST_HOST 1
#include "../../src/battery_monitor.c"

void setUp(void) {}
void tearDown(void) {}

// ---- battery_monitor_v_to_pct: exact LUT points ----

static void test_v_to_pct_4_20(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, battery_monitor_v_to_pct(4.20f)); }
static void test_v_to_pct_4_05(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  90.0f, battery_monitor_v_to_pct(4.05f)); }
static void test_v_to_pct_3_96(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  80.0f, battery_monitor_v_to_pct(3.96f)); }
static void test_v_to_pct_3_90(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  70.0f, battery_monitor_v_to_pct(3.90f)); }
static void test_v_to_pct_3_85(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  60.0f, battery_monitor_v_to_pct(3.85f)); }
static void test_v_to_pct_3_80(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  50.0f, battery_monitor_v_to_pct(3.80f)); }
static void test_v_to_pct_3_76(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  40.0f, battery_monitor_v_to_pct(3.76f)); }
static void test_v_to_pct_3_73(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  30.0f, battery_monitor_v_to_pct(3.73f)); }
static void test_v_to_pct_3_70(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  20.0f, battery_monitor_v_to_pct(3.70f)); }
static void test_v_to_pct_3_65(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,  10.0f, battery_monitor_v_to_pct(3.65f)); }
static void test_v_to_pct_3_20(void) { TEST_ASSERT_FLOAT_WITHIN(0.01f,   0.0f, battery_monitor_v_to_pct(3.20f)); }

// ---- midpoints (linear interpolation) ----

static void test_v_to_pct_midpoint_top(void) {
    // Between 4.20V/100% and 4.05V/90% -> 4.125V -> 95.0%
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 95.0f, battery_monitor_v_to_pct(4.125f));
}
static void test_v_to_pct_midpoint_mid(void) {
    // Between 3.85V/60% and 3.80V/50% -> 3.825V -> 55.0%
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 55.0f, battery_monitor_v_to_pct(3.825f));
}
static void test_v_to_pct_midpoint_low(void) {
    // Between 3.70V/20% and 3.65V/10% -> 3.675V -> 15.0%
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 15.0f, battery_monitor_v_to_pct(3.675f));
}

// ---- clamps ----

static void test_v_to_pct_above_max(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, battery_monitor_v_to_pct(4.30f));
}
static void test_v_to_pct_below_min(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, battery_monitor_v_to_pct(3.00f));
}
static void test_v_to_pct_negative(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, battery_monitor_v_to_pct(-1.0f));
}
static void test_v_to_pct_nan(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, battery_monitor_v_to_pct(NAN));
}

// ---- battery_monitor_is_safe ----

static void test_is_safe_below(void)    { TEST_ASSERT_FALSE(battery_monitor_is_safe(3.69f)); }
static void test_is_safe_boundary(void) { TEST_ASSERT_TRUE(battery_monitor_is_safe(3.70f)); }
static void test_is_safe_above(void)    { TEST_ASSERT_TRUE(battery_monitor_is_safe(3.71f)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v_to_pct_4_20);
    RUN_TEST(test_v_to_pct_4_05);
    RUN_TEST(test_v_to_pct_3_96);
    RUN_TEST(test_v_to_pct_3_90);
    RUN_TEST(test_v_to_pct_3_85);
    RUN_TEST(test_v_to_pct_3_80);
    RUN_TEST(test_v_to_pct_3_76);
    RUN_TEST(test_v_to_pct_3_73);
    RUN_TEST(test_v_to_pct_3_70);
    RUN_TEST(test_v_to_pct_3_65);
    RUN_TEST(test_v_to_pct_3_20);
    RUN_TEST(test_v_to_pct_midpoint_top);
    RUN_TEST(test_v_to_pct_midpoint_mid);
    RUN_TEST(test_v_to_pct_midpoint_low);
    RUN_TEST(test_v_to_pct_above_max);
    RUN_TEST(test_v_to_pct_below_min);
    RUN_TEST(test_v_to_pct_negative);
    RUN_TEST(test_v_to_pct_nan);
    RUN_TEST(test_is_safe_below);
    RUN_TEST(test_is_safe_boundary);
    RUN_TEST(test_is_safe_above);
    return UNITY_END();
}
