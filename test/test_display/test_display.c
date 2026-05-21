#include <unity.h>
#include <string.h>

// Pull in generated assets directly (header is pure data, host-safe).
#include "../../include/display_assets.h"

// Forward-declare the pure helper that this task defines in src/display.c.
// We include the source directly under TEST_HOST to avoid linking ESP-IDF.
#define TEST_HOST 1
#include "../../src/display.c"

void setUp(void) {}
void tearDown(void) {}

// ---- display_battery_v_to_pct ----

static void test_battery_at_floor_is_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, display_battery_v_to_pct(3.3f));
}

static void test_battery_at_ceiling_is_hundred(void) {
    TEST_ASSERT_EQUAL_INT(100, display_battery_v_to_pct(4.2f));
}

static void test_battery_midpoint(void) {
    // 3.75 V is the midpoint between 3.3 and 4.2 -> 50%
    int p = display_battery_v_to_pct(3.75f);
    TEST_ASSERT_TRUE(p >= 49 && p <= 51);
}

static void test_battery_below_floor_clamps_to_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, display_battery_v_to_pct(2.5f));
}

static void test_battery_above_ceiling_clamps_to_hundred(void) {
    TEST_ASSERT_EQUAL_INT(100, display_battery_v_to_pct(4.5f));
}

// ---- asset sanity ----

static void test_qr_bitmap_size(void) {
    // 75x75 px, packed MSB-first into ceil(75/8)=10 bytes per row -> 750 bytes
    TEST_ASSERT_EQUAL_INT(75, DISPLAY_QR_W);
    TEST_ASSERT_EQUAL_INT(75, DISPLAY_QR_H);
    TEST_ASSERT_EQUAL_INT(10, DISPLAY_QR_BPR);
    TEST_ASSERT_EQUAL_INT(750, sizeof(display_qr));
}

static void test_small_font_size(void) {
    // 6x8 -> 1 byte per row -> 8 bytes per glyph; 95 glyphs -> 760 bytes.
    TEST_ASSERT_EQUAL_INT(6, DISPLAY_FONT_SMALL_W);
    TEST_ASSERT_EQUAL_INT(8, DISPLAY_FONT_SMALL_H);
    TEST_ASSERT_EQUAL_INT(95, DISPLAY_FONT_SMALL_COUNT);
    TEST_ASSERT_EQUAL_INT(760, sizeof(display_font_small));
}

static void test_large_font_size(void) {
    // 24x32 -> 3 bytes per row -> 96 bytes per glyph; 12 chars "0123456789.%"
    TEST_ASSERT_EQUAL_INT(24, DISPLAY_FONT_LARGE_W);
    TEST_ASSERT_EQUAL_INT(32, DISPLAY_FONT_LARGE_H);
    TEST_ASSERT_EQUAL_INT(12, (int)strlen(DISPLAY_FONT_LARGE_CHARS));
    TEST_ASSERT_EQUAL_INT(1152, sizeof(display_font_large));
}

static void test_battery_icon_size(void) {
    TEST_ASSERT_EQUAL_INT(16, DISPLAY_ICON_BATTERY_W);
    TEST_ASSERT_EQUAL_INT(10, DISPLAY_ICON_BATTERY_H);
    TEST_ASSERT_EQUAL_INT(20, sizeof(display_icon_battery));
}

static void test_wifi_icon_size(void) {
    TEST_ASSERT_EQUAL_INT(12, DISPLAY_ICON_WIFI_W);
    TEST_ASSERT_EQUAL_INT(10, DISPLAY_ICON_WIFI_H);
    TEST_ASSERT_EQUAL_INT(20, sizeof(display_icon_wifi));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_battery_at_floor_is_zero);
    RUN_TEST(test_battery_at_ceiling_is_hundred);
    RUN_TEST(test_battery_midpoint);
    RUN_TEST(test_battery_below_floor_clamps_to_zero);
    RUN_TEST(test_battery_above_ceiling_clamps_to_hundred);
    RUN_TEST(test_qr_bitmap_size);
    RUN_TEST(test_small_font_size);
    RUN_TEST(test_large_font_size);
    RUN_TEST(test_battery_icon_size);
    RUN_TEST(test_wifi_icon_size);
    return UNITY_END();
}
