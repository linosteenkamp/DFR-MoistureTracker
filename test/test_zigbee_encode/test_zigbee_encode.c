#include <unity.h>
#include <math.h>

#define TEST_HOST 1
#include "../../src/zigbee_encode.c"

void setUp(void) {}
void tearDown(void) {}

// ---- zigbee_encode_soil_pct: uint16, 0.01% units, 0..10000 ----
static void test_soil_zero(void)      { TEST_ASSERT_EQUAL_UINT16(0,     zigbee_encode_soil_pct(0.0f)); }
static void test_soil_full(void)      { TEST_ASSERT_EQUAL_UINT16(10000, zigbee_encode_soil_pct(100.0f)); }
static void test_soil_mid(void)       { TEST_ASSERT_EQUAL_UINT16(4550,  zigbee_encode_soil_pct(45.5f)); }
static void test_soil_fine(void)      { TEST_ASSERT_EQUAL_UINT16(1234,  zigbee_encode_soil_pct(12.34f)); }
static void test_soil_clamp_high(void){ TEST_ASSERT_EQUAL_UINT16(10000, zigbee_encode_soil_pct(150.0f)); }
static void test_soil_negative(void)  { TEST_ASSERT_EQUAL_UINT16(0,     zigbee_encode_soil_pct(-1.0f)); }
static void test_soil_nan(void)       { TEST_ASSERT_EQUAL_UINT16(0,     zigbee_encode_soil_pct(NAN)); }

// ---- zigbee_encode_batt_voltage: uint8, 100mV units ----
static void test_volt_42(void)        { TEST_ASSERT_EQUAL_UINT8(42, zigbee_encode_batt_voltage(4.20f)); }
static void test_volt_37(void)        { TEST_ASSERT_EQUAL_UINT8(37, zigbee_encode_batt_voltage(3.70f)); }
static void test_volt_32(void)        { TEST_ASSERT_EQUAL_UINT8(32, zigbee_encode_batt_voltage(3.20f)); }
static void test_volt_clamp(void)     { TEST_ASSERT_EQUAL_UINT8(255, zigbee_encode_batt_voltage(30.0f)); }
static void test_volt_zero(void)      { TEST_ASSERT_EQUAL_UINT8(0,  zigbee_encode_batt_voltage(0.0f)); }
static void test_volt_nan(void)       { TEST_ASSERT_EQUAL_UINT8(0,  zigbee_encode_batt_voltage(NAN)); }

// ---- zigbee_encode_batt_pct: uint8, 0.5% units, 0..200 ----
static void test_pct_zero(void)       { TEST_ASSERT_EQUAL_UINT8(0,   zigbee_encode_batt_pct(0.0f)); }
static void test_pct_full(void)       { TEST_ASSERT_EQUAL_UINT8(200, zigbee_encode_batt_pct(100.0f)); }
static void test_pct_84(void)         { TEST_ASSERT_EQUAL_UINT8(168, zigbee_encode_batt_pct(84.0f)); }
static void test_pct_half(void)       { TEST_ASSERT_EQUAL_UINT8(100, zigbee_encode_batt_pct(50.0f)); }
static void test_pct_clamp(void)      { TEST_ASSERT_EQUAL_UINT8(200, zigbee_encode_batt_pct(150.0f)); }
static void test_pct_nan(void)        { TEST_ASSERT_EQUAL_UINT8(0,   zigbee_encode_batt_pct(NAN)); }

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_soil_zero);
    RUN_TEST(test_soil_full);
    RUN_TEST(test_soil_mid);
    RUN_TEST(test_soil_fine);
    RUN_TEST(test_soil_clamp_high);
    RUN_TEST(test_soil_negative);
    RUN_TEST(test_soil_nan);
    RUN_TEST(test_volt_42);
    RUN_TEST(test_volt_37);
    RUN_TEST(test_volt_32);
    RUN_TEST(test_volt_clamp);
    RUN_TEST(test_volt_zero);
    RUN_TEST(test_volt_nan);
    RUN_TEST(test_pct_zero);
    RUN_TEST(test_pct_full);
    RUN_TEST(test_pct_84);
    RUN_TEST(test_pct_half);
    RUN_TEST(test_pct_clamp);
    RUN_TEST(test_pct_nan);
    return UNITY_END();
}
