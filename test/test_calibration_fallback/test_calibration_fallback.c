#include <unity.h>
#include <string.h>
#include "../../include/nvs_shim.h"

// ---- in-memory NVS stub ----
#define MAX_ENTRIES 8
static struct { char ns[16]; char key[16]; uint32_t value; bool used; } store[MAX_ENTRIES];

static void store_reset(void) { memset(store, 0, sizeof(store)); }

bool nvs_shim_get_u32(const char *ns, const char *key, uint32_t *out) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (store[i].used && !strcmp(store[i].ns, ns) && !strcmp(store[i].key, key)) {
            *out = store[i].value;
            return true;
        }
    }
    return false;
}

bool nvs_shim_set_u32(const char *ns, const char *key, uint32_t value) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (store[i].used && !strcmp(store[i].ns, ns) && !strcmp(store[i].key, key)) {
            store[i].value = value; return true;
        }
    }
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!store[i].used) {
            store[i].used = true;
            strncpy(store[i].ns, ns, sizeof(store[i].ns) - 1);
            strncpy(store[i].key, key, sizeof(store[i].key) - 1);
            store[i].value = value;
            return true;
        }
    }
    return false;
}

bool nvs_shim_erase_namespace(const char *ns) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (store[i].used && !strcmp(store[i].ns, ns)) memset(&store[i], 0, sizeof(store[i]));
    }
    return true;
}

// SUT
#include "../../src/soil_calibration.c"

void setUp(void) { store_reset(); soil_calibration_init(); }
void tearDown(void) {}

static void test_defaults_when_empty(void) {
    TEST_ASSERT_EQUAL_UINT32(2800, soil_calibration_get_dry_mv());
    TEST_ASSERT_EQUAL_UINT32(0,    soil_calibration_get_wet_mv());
    TEST_ASSERT_EQUAL_UINT32(0,    soil_calibration_get_cal_ts());
}

static void test_save_then_reinit_returns_saved_values(void) {
    TEST_ASSERT_TRUE(soil_calibration_save(2950, 850, 123456));
    soil_calibration_init();
    TEST_ASSERT_EQUAL_UINT32(2950,   soil_calibration_get_dry_mv());
    TEST_ASSERT_EQUAL_UINT32(850,    soil_calibration_get_wet_mv());
    TEST_ASSERT_EQUAL_UINT32(123456, soil_calibration_get_cal_ts());
}

static void test_clear_returns_to_defaults(void) {
    soil_calibration_save(2950, 850, 1);
    TEST_ASSERT_TRUE(soil_calibration_clear());
    soil_calibration_init();
    TEST_ASSERT_EQUAL_UINT32(2800, soil_calibration_get_dry_mv());
    TEST_ASSERT_EQUAL_UINT32(0,    soil_calibration_get_wet_mv());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_when_empty);
    RUN_TEST(test_save_then_reinit_returns_saved_values);
    RUN_TEST(test_clear_returns_to_defaults);
    return UNITY_END();
}
