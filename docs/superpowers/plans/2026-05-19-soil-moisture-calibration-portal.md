# Soil Moisture Calibration Portal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace compile-time soil-moisture calibration with per-device runtime calibration stored in NVS and captured through a SoftAP web portal reachable on first boot and on demand via a GPIO7 wake button.

**Architecture:** Two new modules (`soil_calibration` owns the NVS-backed values, `config_portal` owns the SoftAP + HTTP server) replace the retired `wifi_provisioning` and `factory_reset` modules. `soil_moisture` is refactored to pull calibration at runtime. `main.c` branches on wake cause to choose between the telemetry path and portal mode. Pure-logic units are covered by host-native Unity tests; NVS round-trips by on-device tests.

**Tech Stack:** ESP-IDF 5.x (C), PlatformIO 6, Unity test framework, ESP32-C6 (DFRobot FireBeetle 2).

**Spec:** `docs/superpowers/specs/2026-05-19-soil-moisture-calibration-portal-design.md`

**Reading order:** Tasks are dependency-ordered. Execute top-to-bottom. Each task includes the exact files, code, and commands needed — no prior task context required beyond what is referenced explicitly.

---

## File Structure

### New files

| Path | Responsibility |
|---|---|
| `include/form_parser.h` | Public API for URL-encoded form field extraction. |
| `src/form_parser.c` | Pure C string parser, no ESP-IDF deps — host-testable. |
| `include/nvs_shim.h` | Thin shim over ESP NVS u32 get/set, with a host stub for tests. |
| `src/nvs_shim_esp.c` | Real ESP NVS implementation of the shim. |
| `include/soil_calibration.h` | Public API: init / get_dry_mv / get_wet_mv / save / clear / cal_ts. |
| `src/soil_calibration.c` | Loads/persists calibration via `nvs_shim`; sensible defaults. |
| `include/config_portal.h` | Public API: `config_portal_run()` (blocking). |
| `src/config_portal.c` | SoftAP lifecycle + HTTP server + handlers for all portal endpoints. |
| `test/test_form_parser/test_form_parser.c` | Native unit tests for form parser. |
| `test/test_percentage_math/test_percentage_math.c` | Native unit tests for percentage math. |
| `test/test_calibration_fallback/test_calibration_fallback.c` | Native unit tests for `soil_calibration` defaults + override. |
| `test/test_calibration_nvs/test_calibration_nvs.c` | On-device test for NVS round-trip. |
| `CONFIG_PORTAL.md` | Replaces `WIFI_PROVISIONING.md` + `FACTORY_RESET.md` + calibration section of `SOIL_MOISTURE_SETUP.md`. |

### Modified files

| Path | Change |
|---|---|
| `platformio.ini` | Add `[env:native]`; per-env `test_filter`. |
| `src/CMakeLists.txt` | Swap retired modules for new modules; add `form_parser.c`, `nvs_shim_esp.c`, `soil_calibration.c`, `config_portal.c`. |
| `src/soil_moisture.c` | Extract pure math; pull cal values at runtime; expose `read_raw_mv`. |
| `include/soil_moisture.h` | Add prototype for `soil_moisture_read_raw_mv`. |
| `src/main.c` | Wake-cause branch; `soil_calibration_init` in `init_system`; GPIO7 wake in `enter_deep_sleep`; drop `factory_reset_*` calls. |
| `SOIL_MOISTURE_SETUP.md` | Replace calibration section with portal procedure pointer. |
| `CLAUDE.md` | Update module table + provisioning notes. |
| `README.md` | Update factory-reset and provisioning references if present. |

### Deleted files

| Path |
|---|
| `src/factory_reset.c`, `include/factory_reset.h` |
| `src/wifi_provisioning.c`, `include/wifi_provisioning.h` |
| `FACTORY_RESET.md`, `WIFI_PROVISIONING.md` |

---

## Task 1: Add native PlatformIO test environment

**Files:**
- Modify: `platformio.ini`
- Create: `test/test_smoke/test_smoke.c`

- [ ] **Step 1: Add `[env:native]` to platformio.ini**

Replace the existing `platformio.ini` contents with:

```ini
; PlatformIO Project Configuration File
; Project: DFR-MoistureTracker
; Description: ESP32-C6 soil moisture and battery monitoring system

[env]
platform = espressif32
framework = espidf
monitor_speed = 115200
board_build.partitions = partitions.csv

[env:dfrobot_firebeetle2_esp32c6]
board = dfrobot_firebeetle2_esp32c6
upload_port = /dev/cu.usbmodem83201
monitor_port = /dev/cu.usbmodem83201
test_framework = unity
test_filter = test_calibration_nvs
; Uncomment to disable deep sleep for local testing (stays awake, re-publishes every 5s)
; build_flags = -DDISABLE_DEEP_SLEEP

[env:native]
platform = native
framework =
test_framework = unity
build_flags = -std=c11 -Wall -Wextra -I include
test_filter =
    test_smoke
    test_form_parser
    test_percentage_math
    test_calibration_fallback
```

- [ ] **Step 2: Write a smoke test to prove the native env works**

Create `test/test_smoke/test_smoke.c`:

```c
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_unity_works(void) {
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unity_works);
    return UNITY_END();
}
```

- [ ] **Step 3: Run the smoke test**

Run: `pio test -e native -f test_smoke`
Expected: `1 Tests 0 Failures 0 Ignored`. If PlatformIO complains about a missing compiler, install Xcode command-line tools (`xcode-select --install`) and retry.

- [ ] **Step 4: Verify the firmware env still builds**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS` (compiles unchanged firmware).

- [ ] **Step 5: Commit**

```bash
git add platformio.ini test/test_smoke/test_smoke.c
git commit -m "Add native PlatformIO test environment with Unity smoke test"
```

---

## Task 2: Extract soil-moisture percentage math into a pure function (TDD)

**Files:**
- Modify: `include/soil_moisture.h` (add prototype)
- Modify: `src/soil_moisture.c` (extract pure function, call it from `read_percentage`)
- Create: `test/test_percentage_math/test_percentage_math.c`

- [ ] **Step 1: Write the failing tests**

Create `test/test_percentage_math/test_percentage_math.c`:

```c
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_percentage_math`
Expected: build error — `soil_moisture_calc_percentage` not defined.

- [ ] **Step 3: Add the pure function to `src/soil_moisture.c`**

At the top of `src/soil_moisture.c`, after the `#include "soil_moisture.h"` line, add a host-build guard:

```c
#ifndef TEST_HOST
#include "soil_moisture.h"
#include "adc_manager.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
```

(Replace the existing block of includes with the guarded version above.)

Then add the pure function near the top of the file, outside the guard:

```c
// Pure percentage math, callable from host tests.
// Linear interpolation between dry (0%) and wet (100%) mV thresholds.
float soil_moisture_calc_percentage(int raw_mv, int dry_mv, int wet_mv) {
    if (raw_mv >= dry_mv) return 0.0f;
    if (raw_mv <= wet_mv) return 100.0f;
    float span = (float)(dry_mv - wet_mv);
    if (span <= 0.0f) return 0.0f;
    float pct = 100.0f * (float)(dry_mv - raw_mv) / span;
    if (pct < 0.0f) return 0.0f;
    if (pct > 100.0f) return 100.0f;
    return pct;
}
```

Wrap **all remaining function bodies** (`soil_moisture_init`, `soil_moisture_read_voltage`, `soil_moisture_read_percentage`, `soil_moisture_deinit`) in `#ifndef TEST_HOST … #endif` so the host build only sees the pure function. Place the `#endif` at end of file.

Inside `soil_moisture_read_percentage`, replace the inline if/else interpolation block with a single call:

```c
float percentage = soil_moisture_calc_percentage(voltage_mV, SENSOR_DRY_MV, SENSOR_WET_MV);
```

(Drop the manual `if (voltage_mV >= …)` / clamp block — `calc_percentage` already clamps.)

- [ ] **Step 4: Add the prototype to `include/soil_moisture.h`**

In `include/soil_moisture.h`, add before the closing `#endif`:

```c
/**
 * @brief Pure percentage math from raw ADC mV and calibration mV.
 *
 * Linear interpolation, clamped to [0, 100]. No hardware access.
 * Exposed for unit testing and for direct callers that want the math
 * without triggering a physical read.
 */
float soil_moisture_calc_percentage(int raw_mv, int dry_mv, int wet_mv);
```

- [ ] **Step 5: Run tests — expect PASS**

Run: `pio test -e native -f test_percentage_math`
Expected: `6 Tests 0 Failures 0 Ignored`.

- [ ] **Step 6: Confirm firmware still builds**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`.

- [ ] **Step 7: Commit**

```bash
git add include/soil_moisture.h src/soil_moisture.c test/test_percentage_math/test_percentage_math.c
git commit -m "Extract soil moisture percentage math into pure function with unit tests"
```

---

## Task 3: Extract URL-encoded form parser into its own module (TDD)

**Files:**
- Create: `include/form_parser.h`
- Create: `src/form_parser.c`
- Create: `test/test_form_parser/test_form_parser.c`
- Modify: `src/wifi_provisioning.c` (use the new module)
- Modify: `src/CMakeLists.txt` (register `form_parser.c`)

- [ ] **Step 1: Write the failing tests**

Create `test/test_form_parser/test_form_parser.c`:

```c
#include <unity.h>
#include <string.h>
#include "../../src/form_parser.c"

void setUp(void) {}
void tearDown(void) {}

static void test_extracts_three_fields(void) {
    char ssid[32] = {0}, pw[32] = {0}, dev[32] = {0};
    form_field_t fields[] = {
        {"ssid",      ssid, sizeof(ssid)},
        {"password",  pw,   sizeof(pw)},
        {"device_id", dev,  sizeof(dev)},
    };
    bool ok = form_parser_extract("ssid=foo&password=bar&device_id=baz", fields, 3);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("foo", ssid);
    TEST_ASSERT_EQUAL_STRING("bar", pw);
    TEST_ASSERT_EQUAL_STRING("baz", dev);
}

static void test_returns_false_when_field_missing(void) {
    char ssid[32] = {0}, pw[32] = {0};
    form_field_t fields[] = {
        {"ssid",     ssid, sizeof(ssid)},
        {"password", pw,   sizeof(pw)},
    };
    bool ok = form_parser_extract("ssid=foo", fields, 2);
    TEST_ASSERT_FALSE(ok);
}

static void test_returns_false_when_value_overflows_buffer(void) {
    char ssid[4] = {0};
    form_field_t fields[] = {{"ssid", ssid, sizeof(ssid)}};
    bool ok = form_parser_extract("ssid=toolong", fields, 1);
    TEST_ASSERT_FALSE(ok);
}

static void test_decodes_plus_as_space(void) {
    char ssid[32] = {0};
    form_field_t fields[] = {{"ssid", ssid, sizeof(ssid)}};
    bool ok = form_parser_extract("ssid=my+net", fields, 1);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("my net", ssid);
}

static void test_handles_trailing_field(void) {
    char a[8] = {0}, b[8] = {0};
    form_field_t fields[] = {{"a", a, sizeof(a)}, {"b", b, sizeof(b)}};
    bool ok = form_parser_extract("a=x&b=y", fields, 2);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("x", a);
    TEST_ASSERT_EQUAL_STRING("y", b);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_extracts_three_fields);
    RUN_TEST(test_returns_false_when_field_missing);
    RUN_TEST(test_returns_false_when_value_overflows_buffer);
    RUN_TEST(test_decodes_plus_as_space);
    RUN_TEST(test_handles_trailing_field);
    return UNITY_END();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_form_parser`
Expected: build error — `form_parser.c` doesn't exist.

- [ ] **Step 3: Create `include/form_parser.h`**

```c
#ifndef FORM_PARSER_H
#define FORM_PARSER_H

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief One field expected in a URL-encoded form body.
 *
 * `name`    — field name, e.g. "ssid"
 * `dst`     — destination buffer for the decoded value
 * `dst_len` — capacity of `dst` in bytes (must include room for NUL)
 */
typedef struct {
    const char *name;
    char *dst;
    size_t dst_len;
} form_field_t;

/**
 * @brief Extract a set of fields from a URL-encoded form body.
 *
 * For each field, locates `name=...` in `body`, copies the value into
 * `dst`, and decodes `+` to space. Returns false if any field is
 * missing or would overflow its destination buffer.
 *
 * NOTE: %XX percent-decoding is not implemented (current SoftAP form
 * does not need it). Add a TODO test before implementing.
 */
bool form_parser_extract(const char *body, const form_field_t *fields, size_t n_fields);

#endif
```

- [ ] **Step 4: Create `src/form_parser.c`**

```c
#include "form_parser.h"
#include <string.h>

static void decode_plus_to_space(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == '+') *p = ' ';
    }
}

static bool extract_one(const char *body, const form_field_t *f) {
    // Build "<name>=" search prefix on the stack (max ~32 bytes).
    char needle[40];
    size_t name_len = strlen(f->name);
    if (name_len + 2 > sizeof(needle)) return false;
    memcpy(needle, f->name, name_len);
    needle[name_len] = '=';
    needle[name_len + 1] = '\0';

    const char *start = strstr(body, needle);
    if (!start) return false;
    start += name_len + 1;

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= f->dst_len) return false;

    memcpy(f->dst, start, len);
    f->dst[len] = '\0';
    decode_plus_to_space(f->dst);
    return true;
}

bool form_parser_extract(const char *body, const form_field_t *fields, size_t n_fields) {
    if (!body || !fields) return false;
    for (size_t i = 0; i < n_fields; i++) {
        if (!extract_one(body, &fields[i])) return false;
    }
    return true;
}
```

- [ ] **Step 5: Run tests — expect PASS**

Run: `pio test -e native -f test_form_parser`
Expected: `5 Tests 0 Failures 0 Ignored`.

- [ ] **Step 6: Use the new module from `wifi_provisioning.c`**

In `src/wifi_provisioning.c`:

- Add `#include "form_parser.h"` near the top.
- **Delete** the existing `url_decode`, `copy_form_field`, and `parse_form_fields` functions (lines 47-88).
- Replace the call site inside `save_post_handler` (the `if (!parse_form_fields(...))` block) with:

```c
form_field_t fields[] = {
    {"ssid",      ssid,      sizeof(ssid)},
    {"password",  password,  sizeof(password)},
    {"device_id", device_id, sizeof(device_id)},
};
if (!form_parser_extract(buf, fields, 3)) {
    free(buf);
    ESP_LOGE(TAG, "Failed to parse form fields");
    httpd_resp_send_500(req);
    return ESP_FAIL;
}
```

- [ ] **Step 7: Register `form_parser.c` in `src/CMakeLists.txt`**

Add `"form_parser.c"` to the `SRCS` list (sorted; place after `factory_reset.c`).

- [ ] **Step 8: Confirm firmware still builds**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`.

- [ ] **Step 9: Commit**

```bash
git add include/form_parser.h src/form_parser.c src/wifi_provisioning.c src/CMakeLists.txt test/test_form_parser/test_form_parser.c
git commit -m "Extract URL-encoded form parser into its own module with unit tests"
```

---

## Task 4: Define NVS shim (interface + ESP implementation)

**Files:**
- Create: `include/nvs_shim.h`
- Create: `src/nvs_shim_esp.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create `include/nvs_shim.h`**

```c
#ifndef NVS_SHIM_H
#define NVS_SHIM_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Thin wrapper around ESP NVS u32 get/set + namespace erase.
 *
 * Exists so modules that only need u32 storage (e.g. soil_calibration)
 * can be unit-tested on the host by linking against an in-memory stub
 * implementation. The ESP build links nvs_shim_esp.c; native tests
 * link an in-test stub defined alongside the test file.
 */

/** Get a u32 value. Returns false if namespace/key absent. */
bool nvs_shim_get_u32(const char *ns, const char *key, uint32_t *out);

/** Set a u32 value, commit immediately. Returns false on failure. */
bool nvs_shim_set_u32(const char *ns, const char *key, uint32_t value);

/** Erase all keys in a namespace. Returns false on failure. */
bool nvs_shim_erase_namespace(const char *ns);

#endif
```

- [ ] **Step 2: Create `src/nvs_shim_esp.c`**

```c
#include "nvs_shim.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "NVS_SHIM";

bool nvs_shim_get_u32(const char *ns, const char *key, uint32_t *out) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_u32(h, key, out);
    nvs_close(h);
    return err == ESP_OK;
}

bool nvs_shim_set_u32(const char *ns, const char *key, uint32_t value) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "open %s/%s failed", ns, key);
        return false;
    }
    bool ok = (nvs_set_u32(h, key, value) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool nvs_shim_erase_namespace(const char *ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_erase_all(h) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}
```

- [ ] **Step 3: Register `nvs_shim_esp.c` in `src/CMakeLists.txt`**

Add `"nvs_shim_esp.c"` to the `SRCS` list (alphabetic placement after `mqtt_publisher.c`).

- [ ] **Step 4: Confirm firmware still builds**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS` (no callers yet — just compiles in).

- [ ] **Step 5: Commit**

```bash
git add include/nvs_shim.h src/nvs_shim_esp.c src/CMakeLists.txt
git commit -m "Add NVS u32 shim for host-testable calibration storage"
```

---

## Task 5: Create `soil_calibration` module with host fallback test (TDD)

**Files:**
- Create: `include/soil_calibration.h`
- Create: `src/soil_calibration.c`
- Create: `test/test_calibration_fallback/test_calibration_fallback.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `test/test_calibration_fallback/test_calibration_fallback.c`:

```c
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_calibration_fallback`
Expected: build error — `soil_calibration.c` doesn't exist.

- [ ] **Step 3: Create `include/soil_calibration.h`**

```c
#ifndef SOIL_CALIBRATION_H
#define SOIL_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Runtime soil-moisture calibration values.
 *
 * Owns the per-device dry/wet mV thresholds and the last-cal timestamp.
 * Backed by NVS namespace "soil_cal" through nvs_shim.
 *
 * Defaults if NVS has no values:
 *   dry_mv = 2800, wet_mv = 0, cal_ts = 0
 */

/** Load from NVS into RAM. Falls back to defaults on missing keys. */
void soil_calibration_init(void);

uint32_t soil_calibration_get_dry_mv(void);
uint32_t soil_calibration_get_wet_mv(void);
uint32_t soil_calibration_get_cal_ts(void);

/** Persist values to NVS and update in-RAM cache. */
bool soil_calibration_save(uint32_t dry_mv, uint32_t wet_mv, uint32_t cal_ts);

/** Erase the whole "soil_cal" namespace. RAM cache reverts on next init. */
bool soil_calibration_clear(void);

#endif
```

- [ ] **Step 4: Create `src/soil_calibration.c`**

```c
#include "soil_calibration.h"
#include "nvs_shim.h"

#define NS          "soil_cal"
#define KEY_DRY     "dry_mv"
#define KEY_WET     "wet_mv"
#define KEY_TS      "cal_ts"

#define DEFAULT_DRY 2800
#define DEFAULT_WET 0
#define DEFAULT_TS  0

static uint32_t s_dry = DEFAULT_DRY;
static uint32_t s_wet = DEFAULT_WET;
static uint32_t s_ts  = DEFAULT_TS;

void soil_calibration_init(void) {
    uint32_t v;
    s_dry = nvs_shim_get_u32(NS, KEY_DRY, &v) ? v : DEFAULT_DRY;
    s_wet = nvs_shim_get_u32(NS, KEY_WET, &v) ? v : DEFAULT_WET;
    s_ts  = nvs_shim_get_u32(NS, KEY_TS,  &v) ? v : DEFAULT_TS;
}

uint32_t soil_calibration_get_dry_mv(void) { return s_dry; }
uint32_t soil_calibration_get_wet_mv(void) { return s_wet; }
uint32_t soil_calibration_get_cal_ts(void) { return s_ts;  }

bool soil_calibration_save(uint32_t dry_mv, uint32_t wet_mv, uint32_t cal_ts) {
    if (!nvs_shim_set_u32(NS, KEY_DRY, dry_mv)) return false;
    if (!nvs_shim_set_u32(NS, KEY_WET, wet_mv)) return false;
    if (!nvs_shim_set_u32(NS, KEY_TS,  cal_ts)) return false;
    s_dry = dry_mv; s_wet = wet_mv; s_ts = cal_ts;
    return true;
}

bool soil_calibration_clear(void) {
    return nvs_shim_erase_namespace(NS);
}
```

- [ ] **Step 5: Run tests — expect PASS**

Run: `pio test -e native -f test_calibration_fallback`
Expected: `3 Tests 0 Failures 0 Ignored`.

- [ ] **Step 6: Register `soil_calibration.c` in `src/CMakeLists.txt`**

Add `"soil_calibration.c"` to `SRCS` (after `soil_moisture.c`).

- [ ] **Step 7: Confirm firmware still builds**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`.

- [ ] **Step 8: Commit**

```bash
git add include/soil_calibration.h src/soil_calibration.c src/CMakeLists.txt test/test_calibration_fallback/test_calibration_fallback.c
git commit -m "Add soil_calibration module with NVS-backed runtime values"
```

---

## Task 6: On-device NVS round-trip test for `soil_calibration`

**Files:**
- Create: `test/test_calibration_nvs/test_calibration_nvs.c`

- [ ] **Step 1: Write the on-device test**

Create `test/test_calibration_nvs/test_calibration_nvs.c`:

```c
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
```

- [ ] **Step 2: Run the on-device test**

Plug in the board. Run:
```bash
pio test -e dfrobot_firebeetle2_esp32c6 -f test_calibration_nvs
```
Expected: `3 Tests 0 Failures 0 Ignored`. PlatformIO will build, flash, run, and read back the test output over serial.

- [ ] **Step 3: Commit**

```bash
git add test/test_calibration_nvs/test_calibration_nvs.c
git commit -m "Add on-device NVS round-trip test for soil_calibration"
```

---

## Task 7: Refactor `soil_moisture` to use `soil_calibration` at runtime; expose `read_raw_mv`

**Files:**
- Modify: `include/soil_moisture.h`
- Modify: `src/soil_moisture.c`

- [ ] **Step 1: Add the prototype to `include/soil_moisture.h`**

Before the closing `#endif`, add:

```c
/**
 * @brief Read averaged raw sensor value in millivolts.
 *
 * Like soil_moisture_read_voltage() but returns the integer mV from
 * the same 10-sample average. Used by the calibration capture
 * endpoints in config_portal.
 *
 * @return mV (0 if sensor not initialized or all reads fail)
 */
int soil_moisture_read_raw_mv(void);
```

- [ ] **Step 2: Modify `src/soil_moisture.c`**

Inside the `#ifndef TEST_HOST` block (added in Task 2):

- Add at the top with the other includes:
  ```c
  #include "soil_calibration.h"
  ```
- **Delete** the lines:
  ```c
  #define SENSOR_DRY_MV         2800
  #define SENSOR_WET_MV         0
  ```
- Update the call inside `soil_moisture_read_percentage` from:
  ```c
  float percentage = soil_moisture_calc_percentage(voltage_mV, SENSOR_DRY_MV, SENSOR_WET_MV);
  ```
  to:
  ```c
  float percentage = soil_moisture_calc_percentage(
      voltage_mV,
      (int)soil_calibration_get_dry_mv(),
      (int)soil_calibration_get_wet_mv());
  ```
- Update the init log line from:
  ```c
  ESP_LOGI(TAG, "Calibration: Dry=%d mV, Wet=%d mV", SENSOR_DRY_MV, SENSOR_WET_MV);
  ```
  to:
  ```c
  ESP_LOGI(TAG, "Calibration (runtime): Dry=%u mV, Wet=%u mV",
           (unsigned)soil_calibration_get_dry_mv(),
           (unsigned)soil_calibration_get_wet_mv());
  ```

- [ ] **Step 3: Refactor `soil_moisture_read_voltage` to share its sampling code with `read_raw_mv`**

Add a new static helper above `soil_moisture_read_voltage`:

```c
// Returns averaged sensor mV, or -1 on hard failure.
static int sample_raw_mv(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return -1;
    }
    adc_oneshot_unit_handle_t adc_handle = adc_manager_get_handle();
    if (!adc_handle) {
        ESP_LOGE(TAG, "ADC handle not available");
        return -1;
    }
    gpio_set_level(SOIL_PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(SOIL_WARMUP_MS));

    uint32_t sum = 0;
    int n = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc_handle, SOIL_ADC_CHAN, &raw) == ESP_OK) {
            sum += raw; n++;
        }
    }
    gpio_set_level(SOIL_PWR_GPIO, 0);
    if (n == 0) return -1;

    int mv = 0;
    if (adc_cali_raw_to_voltage(cali_handle, (int)(sum / n), &mv) != ESP_OK) return -1;
    return mv;
}
```

Replace the body of `soil_moisture_read_voltage` with:

```c
float soil_moisture_read_voltage(void) {
    int mv = sample_raw_mv();
    if (mv < 0) return 0.0f;
    ESP_LOGD(TAG, "Raw mV: %d", mv);
    return (float)mv / 1000.0f;
}
```

Add the new public function:

```c
int soil_moisture_read_raw_mv(void) {
    int mv = sample_raw_mv();
    return mv < 0 ? 0 : mv;
}
```

- [ ] **Step 4: Run the host tests to make sure nothing in the pure path regressed**

Run: `pio test -e native`
Expected: all native tests pass.

- [ ] **Step 5: Build the firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`.

- [ ] **Step 6: Commit**

```bash
git add include/soil_moisture.h src/soil_moisture.c
git commit -m "Pull soil moisture calibration from runtime values; expose read_raw_mv"
```

---

## Task 8: Create `config_portal` module — SoftAP scaffold + main menu

**Files:**
- Create: `include/config_portal.h`
- Create: `src/config_portal.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create `include/config_portal.h`**

```c
#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include "esp_err.h"

/**
 * @brief Run the SoftAP configuration portal, blocking.
 *
 * Brings up the FireBeetle_C6_Prov AP and an HTTP server. Serves the
 * config menu (WiFi, calibration, status, factory reset). Returns when:
 *  - A handler calls esp_restart() (it never returns), or
 *  - The 10-minute idle timeout fires (returns ESP_OK).
 *
 * Caller is expected to deep-sleep afterwards.
 */
esp_err_t config_portal_run(void);

#endif
```

- [ ] **Step 2: Create `src/config_portal.c` with SoftAP + root handler only**

This task creates the skeleton — WiFi/calibration/status/reset handlers land in Tasks 9–11.

```c
#include "config_portal.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "CONFIG_PORTAL";
static httpd_handle_t s_server = NULL;
static bool s_should_exit = false;
static int  s_idle_ticks = 0;

#define PROV_AP_SSID         "FireBeetle_C6_Prov"
#define PORTAL_TIMEOUT_SEC   600
#define IDLE_TICK_MS         1000

static const char *html_menu =
    "<!DOCTYPE html><html><head><title>FireBeetle Config</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
    "a.btn{display:block;padding:14px;margin:10px 0;background:#4CAF50;color:white;"
    "text-align:center;text-decoration:none;border-radius:4px}"
    "a.btn.danger{background:#d9534f}</style></head>"
    "<body><div class='container'><h2>FireBeetle C6</h2>"
    "<a class='btn' href='/wifi'>WiFi &amp; Device ID</a>"
    "<a class='btn' href='/calibrate'>Calibrate Sensor</a>"
    "<a class='btn' href='/status'>Status</a>"
    "<a class='btn danger' href='/factory-reset'>Factory Reset</a>"
    "</div></body></html>";

static esp_err_t root_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_menu, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_softap(void) {
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = PROV_AP_SSID,
            .ssid_len = strlen(PROV_AP_SSID),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "AP up: %s — open http://192.168.4.1", PROV_AP_SSID);
    return ESP_OK;
}

static esp_err_t start_http(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &root);

    // (Other handlers registered in Tasks 9–11.)
    return ESP_OK;
}

static void stop_server(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
}

esp_err_t config_portal_run(void) {
    ESP_LOGI(TAG, "Starting config portal");
    s_should_exit = false;
    s_idle_ticks = 0;

    esp_err_t err = start_softap();
    if (err != ESP_OK) { stop_server(); return err; }
    err = start_http();
    if (err != ESP_OK) { stop_server(); return err; }

    while (!s_should_exit && s_idle_ticks < PORTAL_TIMEOUT_SEC) {
        vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
        s_idle_ticks++;
    }

    ESP_LOGI(TAG, "Portal exiting (timeout=%d should_exit=%d)",
             s_idle_ticks >= PORTAL_TIMEOUT_SEC, s_should_exit);
    stop_server();
    return ESP_OK;
}
```

- [ ] **Step 3: Register `config_portal.c` in `src/CMakeLists.txt`**

Add `"config_portal.c"` (place after `factory_reset.c`).

- [ ] **Step 4: Build firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`. Module compiles but isn't called yet (Task 12 wires it into `main.c`).

- [ ] **Step 5: Commit**

```bash
git add include/config_portal.h src/config_portal.c src/CMakeLists.txt
git commit -m "Add config_portal scaffold with SoftAP and main menu"
```

---

## Task 9: Add `/wifi` GET/POST handlers to config portal

**Files:**
- Modify: `src/config_portal.c`

- [ ] **Step 1: Add WiFi handlers to `src/config_portal.c`**

At the top of the file, alongside the existing includes, add:

```c
#include "wifi_credentials.h"
#include "form_parser.h"
#include <stdlib.h>
```

Add HTML constants below `html_menu`:

```c
static const char *html_wifi_form =
    "<!DOCTYPE html><html><head><title>WiFi Setup</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
    "input{width:100%;padding:10px;margin:10px 0;box-sizing:border-box}"
    "button{background:#4CAF50;color:white;padding:14px;border:none;width:100%;cursor:pointer;font-size:16px}"
    "a{display:block;text-align:center;margin-top:14px}</style></head>"
    "<body><div class='container'><h2>WiFi &amp; Device ID</h2>"
    "<form action='/wifi' method='POST'>"
    "<label>SSID:</label><input type='text' name='ssid' required>"
    "<label>Password:</label><input type='password' name='password' required>"
    "<label>Device ID:</label><input type='text' name='device_id' placeholder='moisture01' required>"
    "<button type='submit'>Save &amp; Restart</button></form>"
    "<a href='/'>Back</a></div></body></html>";

static const char *html_wifi_saved =
    "<!DOCTYPE html><html><body><h1>WiFi saved.</h1>"
    "<p>Device will restart in 2 seconds.</p></body></html>";
```

Add the handlers above `start_http()`:

```c
static esp_err_t wifi_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_wifi_form, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_post(httpd_req_t *req) {
    s_idle_ticks = 0;
    int total = req->content_len;
    if (total <= 0 || total > 1024) { httpd_resp_send_500(req); return ESP_FAIL; }
    char *buf = malloc(total + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        got += r;
    }
    buf[total] = '\0';

    char ssid[33] = {0}, password[65] = {0}, device_id[33] = {0};
    form_field_t fields[] = {
        {"ssid",      ssid,      sizeof(ssid)},
        {"password",  password,  sizeof(password)},
        {"device_id", device_id, sizeof(device_id)},
    };
    bool ok = form_parser_extract(buf, fields, 3);
    free(buf);
    if (!ok) { httpd_resp_send_500(req); return ESP_FAIL; }

    if (wifi_credentials_save(ssid, password) != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (wifi_credentials_save_device_id(device_id) != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_wifi_saved, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}
```

Register them inside `start_http()`, after the root handler:

```c
httpd_uri_t wifi_g = {.uri = "/wifi", .method = HTTP_GET,  .handler = wifi_get,  .user_ctx = NULL};
httpd_uri_t wifi_p = {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post, .user_ctx = NULL};
httpd_register_uri_handler(s_server, &wifi_g);
httpd_register_uri_handler(s_server, &wifi_p);
```

- [ ] **Step 2: Build firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add src/config_portal.c
git commit -m "Add /wifi handlers to config portal"
```

---

## Task 10: Add `/calibrate` page and `/api/*` calibration endpoints

**Files:**
- Modify: `src/config_portal.c`

- [ ] **Step 1: Add calibration handlers**

Add to the include block at the top of `config_portal.c`:

```c
#include "soil_calibration.h"
#include "soil_moisture.h"
#include <stdio.h>
#include "esp_timer.h"
```

Add pending-capture state (file-static), near the other statics:

```c
static int s_pending_dry_mv = -1;
static int s_pending_wet_mv = -1;
```

Add HTML constant after `html_wifi_saved`:

```c
static const char *html_calibrate =
    "<!DOCTYPE html><html><head><title>Calibrate</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".c{background:white;padding:30px;border-radius:10px;max-width:480px;margin:auto}"
    ".live{font-size:32px;text-align:center;padding:14px;background:#eef;border-radius:6px;margin:14px 0}"
    "button{background:#4CAF50;color:white;padding:12px;border:none;width:100%;cursor:pointer;font-size:15px;margin:6px 0}"
    ".captured{padding:8px;background:#dfd;border-radius:4px;text-align:center}"
    "a{display:block;text-align:center;margin-top:14px}</style></head>"
    "<body><div class='c'><h2>Calibrate Sensor</h2>"
    "<div class='live' id='live'>… mV</div>"
    "<p>1. Hold sensor in <b>open air</b>, then:</p>"
    "<button onclick='cap(\"dry\")'>Capture DRY</button>"
    "<div class='captured' id='dry'>not captured</div>"
    "<p>2. Submerge sensor to MAX line, then:</p>"
    "<button onclick='cap(\"wet\")'>Capture WET</button>"
    "<div class='captured' id='wet'>not captured</div>"
    "<button onclick='save()'>Save &amp; Restart</button>"
    "<a href='/'>Back</a></div>"
    "<script>"
    "async function poll(){try{let r=await fetch('/api/reading');let j=await r.json();"
    "document.getElementById('live').textContent=j.raw_mv+' mV ('+j.percentage.toFixed(1)+'%)';}catch(e){}}"
    "setInterval(poll,1000);poll();"
    "async function cap(k){let r=await fetch('/api/calibrate/'+k,{method:'POST'});let j=await r.json();"
    "document.getElementById(k).textContent='captured: '+j.mv+' mV';}"
    "async function save(){let r=await fetch('/api/calibrate/save',{method:'POST'});"
    "if(r.ok){document.body.innerHTML='<h1>Saved.</h1>';setTimeout(()=>location.href='/',1500);}"
    "else{alert('Capture both DRY and WET first.');}}"
    "</script></body></html>";
```

Add the handlers above `start_http()`:

```c
static esp_err_t calibrate_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_calibrate, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_reading_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    int raw = soil_moisture_read_raw_mv();
    uint32_t dry = soil_calibration_get_dry_mv();
    uint32_t wet = soil_calibration_get_wet_mv();
    float pct = soil_moisture_calc_percentage(raw, (int)dry, (int)wet);

    char body[160];
    snprintf(body, sizeof(body),
        "{\"raw_mv\":%d,\"percentage\":%.1f,\"dry_mv\":%u,\"wet_mv\":%u}",
        raw, pct, (unsigned)dry, (unsigned)wet);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_capture(httpd_req_t *req, int *target) {
    s_idle_ticks = 0;
    int mv = soil_moisture_read_raw_mv();
    *target = mv;
    char body[40];
    snprintf(body, sizeof(body), "{\"mv\":%d}", mv);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_calibrate_dry(httpd_req_t *req) {
    return api_capture(req, &s_pending_dry_mv);
}
static esp_err_t api_calibrate_wet(httpd_req_t *req) {
    return api_capture(req, &s_pending_wet_mv);
}

static esp_err_t api_calibrate_save(httpd_req_t *req) {
    s_idle_ticks = 0;
    if (s_pending_dry_mv < 0 || s_pending_wet_mv < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "capture dry and wet first");
        return ESP_FAIL;
    }
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000);  // seconds since boot
    bool ok = soil_calibration_save(
        (uint32_t)s_pending_dry_mv, (uint32_t)s_pending_wet_mv, ts);
    if (!ok) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}
```

Register them inside `start_http()`, after the `/wifi` registrations:

```c
httpd_uri_t cal_g  = {.uri = "/calibrate",        .method = HTTP_GET,  .handler = calibrate_get};
httpd_uri_t cal_r  = {.uri = "/api/reading",      .method = HTTP_GET,  .handler = api_reading_get};
httpd_uri_t cal_d  = {.uri = "/api/calibrate/dry",.method = HTTP_POST, .handler = api_calibrate_dry};
httpd_uri_t cal_w  = {.uri = "/api/calibrate/wet",.method = HTTP_POST, .handler = api_calibrate_wet};
httpd_uri_t cal_s  = {.uri = "/api/calibrate/save",.method = HTTP_POST,.handler = api_calibrate_save};
httpd_register_uri_handler(s_server, &cal_g);
httpd_register_uri_handler(s_server, &cal_r);
httpd_register_uri_handler(s_server, &cal_d);
httpd_register_uri_handler(s_server, &cal_w);
httpd_register_uri_handler(s_server, &cal_s);
```

- [ ] **Step 2: Build firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add src/config_portal.c
git commit -m "Add /calibrate page and capture/save API endpoints"
```

---

## Task 11: Add `/status` and `/factory-reset` handlers

**Files:**
- Modify: `src/config_portal.c`

- [ ] **Step 1: Add status + factory-reset handlers**

Add HTML constant after `html_calibrate`:

```c
static const char *html_reset_confirm =
    "<!DOCTYPE html><html><head><title>Factory Reset</title>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".c{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto;text-align:center}"
    "button{background:#d9534f;color:white;padding:14px;border:none;width:100%;cursor:pointer;font-size:16px;margin:8px 0}"
    "a{display:block;margin-top:14px}</style></head>"
    "<body><div class='c'><h2>Factory Reset</h2>"
    "<p>This wipes WiFi credentials and calibration. Device will restart and ask for setup again.</p>"
    "<form action='/factory-reset' method='POST'>"
    "<button type='submit'>Yes, wipe everything</button></form>"
    "<a href='/'>Cancel</a></div></body></html>";
```

Add handlers above `start_http()`:

```c
static esp_err_t status_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    int raw = soil_moisture_read_raw_mv();
    uint32_t dry = soil_calibration_get_dry_mv();
    uint32_t wet = soil_calibration_get_wet_mv();
    uint32_t ts  = soil_calibration_get_cal_ts();
    float pct = soil_moisture_calc_percentage(raw, (int)dry, (int)wet);

    char body[768];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head><title>Status</title>"
        "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
        ".c{background:white;padding:30px;border-radius:10px;max-width:480px;margin:auto}"
        "table{width:100%%}td{padding:6px 0}td.k{color:#666;width:40%%}"
        "a{display:block;text-align:center;margin-top:14px}</style></head>"
        "<body><div class='c'><h2>Status</h2><table>"
        "<tr><td class='k'>DRY mV</td><td>%u</td></tr>"
        "<tr><td class='k'>WET mV</td><td>%u</td></tr>"
        "<tr><td class='k'>Last cal (s)</td><td>%u</td></tr>"
        "<tr><td class='k'>Live mV</td><td>%d</td></tr>"
        "<tr><td class='k'>Live %%</td><td>%.1f</td></tr>"
        "</table><a href='/'>Back</a></div></body></html>",
        (unsigned)dry, (unsigned)wet, (unsigned)ts, raw, pct);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t factory_reset_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_reset_confirm, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t factory_reset_post(httpd_req_t *req) {
    s_idle_ticks = 0;
    wifi_credentials_clear();
    soil_calibration_clear();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<html><body><h1>Wiped. Restarting…</h1></body></html>",
        HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}
```

Register inside `start_http()`, after the calibration registrations:

```c
httpd_uri_t st_g  = {.uri = "/status",         .method = HTTP_GET,  .handler = status_get};
httpd_uri_t fr_g  = {.uri = "/factory-reset",  .method = HTTP_GET,  .handler = factory_reset_get};
httpd_uri_t fr_p  = {.uri = "/factory-reset",  .method = HTTP_POST, .handler = factory_reset_post};
httpd_register_uri_handler(s_server, &st_g);
httpd_register_uri_handler(s_server, &fr_g);
httpd_register_uri_handler(s_server, &fr_p);
```

- [ ] **Step 2: Build firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`.

- [ ] **Step 3: Commit**

```bash
git add src/config_portal.c
git commit -m "Add /status and /factory-reset handlers to config portal"
```

---

## Task 12: Wire `main.c` to wake-cause branch + GPIO7 wake

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Replace `wifi_provisioning` + `factory_reset` includes with `config_portal` + `soil_calibration`**

In `src/main.c`, change the include block from:

```c
#include "wifi_credentials.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"
#include "adc_manager.h"
#include "battery_monitor.h"
#include "soil_moisture.h"
#include "mqtt_publisher.h"
#include "factory_reset.h"
#include "mqtt_credentials.h"
```

to:

```c
#include "wifi_credentials.h"
#include "wifi_manager.h"
#include "config_portal.h"
#include "adc_manager.h"
#include "battery_monitor.h"
#include "soil_moisture.h"
#include "soil_calibration.h"
#include "mqtt_publisher.h"
#include "mqtt_credentials.h"
```

Also add to the system headers near the top:

```c
#include "driver/rtc_io.h"   // already present
#include "esp_sleep.h"        // already present — GPIO wake API
```

(Both are likely already there from existing deep-sleep code; verify and add only if missing.)

- [ ] **Step 2: Replace the `factory_reset_init` call with `soil_calibration_init`**

In `init_system()`, find:

```c
// Initialize factory reset button
ESP_LOGI(TAG, "Initializing factory reset button...");
ret = factory_reset_init();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize factory reset button");
    return ret;
}
ESP_LOGI(TAG, "Factory reset button initialized");
```

Replace the entire block with:

```c
// Initialize soil calibration (loads NVS values or falls back to defaults)
ESP_LOGI(TAG, "Initializing soil calibration...");
soil_calibration_init();
ESP_LOGI(TAG, "Soil calibration loaded");
```

`soil_calibration_init` returns void and never fails — it falls back to defaults. Order matters: it must run **before** `soil_moisture_init` (which already happens later in `init_system`).

- [ ] **Step 3: Remove the old `handle_provisioning`/`setup_wifi`-driven provisioning helper**

In `setup_wifi()`, find:

```c
if (!wifi_credentials_is_provisioned()) {
    ESP_LOGI(TAG, "Device not provisioned");
    return handle_provisioning();
}
```

Delete that block (the wake-cause branch in `app_main` now handles unprovisioned boots by entering the portal directly). `setup_wifi()` should now begin straight at `wifi_manager_init_sta()`.

Delete the `handle_provisioning` function definition entirely.

- [ ] **Step 4: Add a portal-mode helper and the wake-cause branch in `app_main`**

Add this helper above `app_main`:

```c
static void run_portal_then_sleep(void) {
    ESP_LOGI(TAG, "Entering config portal");
    config_portal_run();   // blocks until save or timeout
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
}
```

In `app_main`, after the wake-cause logging switch and before `init_system()`, leave the call order as-is. Then **after** `init_system()` succeeds, add a branch *before* `setup_wifi()`:

```c
// Portal mode triggers: GPIO wake (button press) or never-provisioned device.
if (wake_cause == ESP_SLEEP_WAKEUP_GPIO || !wifi_credentials_is_provisioned()) {
    run_portal_then_sleep();
    return;
}
```

- [ ] **Step 5: Configure GPIO7 deep-sleep wake inside `enter_deep_sleep`**

In `enter_deep_sleep`, just before `esp_deep_sleep_start()`, add:

```c
// GPIO7 = config-portal wake button (LP-capable, momentary push-to-GND, internal pull-up).
gpio_config_t btn_conf = {
    .pin_bit_mask = (1ULL << GPIO_NUM_7),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
};
gpio_config(&btn_conf);
esp_deep_sleep_enable_gpio_wakeup(BIT(GPIO_NUM_7), ESP_GPIO_WAKEUP_GPIO_LOW);
```

Keep all other existing teardown (mqtt/wifi stop, GPIO20 float, rtc isolation, timer wake, gpio_hold_en for GPIO3) unchanged.

- [ ] **Step 6: Handle the new wake-cause case in the logging switch**

In `app_main`, find the wake-cause logging switch (`switch (wake_cause)`). Add a case before `default`:

```c
case ESP_SLEEP_WAKEUP_GPIO:
    ESP_LOGI(TAG, "Wake cause: GPIO button");
    break;
```

- [ ] **Step 7: Build firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`. Linker will warn/error if anything still references retired symbols; if so, return to that reference and remove it.

- [ ] **Step 8: Commit**

```bash
git add src/main.c
git commit -m "Wire main to wake-cause branch and GPIO7 portal wake"
```

---

## Task 13: Retire `factory_reset` and `wifi_provisioning` modules

**Files:**
- Delete: `src/factory_reset.c`, `include/factory_reset.h`
- Delete: `src/wifi_provisioning.c`, `include/wifi_provisioning.h`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Delete the retired sources**

```bash
git rm src/factory_reset.c include/factory_reset.h
git rm src/wifi_provisioning.c include/wifi_provisioning.h
```

- [ ] **Step 2: Remove them from `src/CMakeLists.txt`**

In `src/CMakeLists.txt`, delete the lines:

```
        "wifi_provisioning.c"
        "factory_reset.c"
```

The final `SRCS` list should read (alphabetised):

```
        "adc_manager.c"
        "battery_monitor.c"
        "config_portal.c"
        "form_parser.c"
        "main.c"
        "mqtt_publisher.c"
        "nvs_shim_esp.c"
        "soil_calibration.c"
        "soil_moisture.c"
        "wifi_credentials.c"
        "wifi_manager.c"
```

- [ ] **Step 3: Build firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6`
Expected: `SUCCESS`. If unresolved symbols appear, search the project (`grep -r factory_reset src include`) and remove stragglers.

- [ ] **Step 4: Run the full native test suite**

Run: `pio test -e native`
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/CMakeLists.txt
git commit -m "Retire factory_reset and wifi_provisioning modules"
```

---

## Task 14: Documentation updates

**Files:**
- Create: `CONFIG_PORTAL.md`
- Delete: `FACTORY_RESET.md`, `WIFI_PROVISIONING.md`
- Modify: `SOIL_MOISTURE_SETUP.md`, `CLAUDE.md`, `README.md`

- [ ] **Step 1: Create `CONFIG_PORTAL.md`**

```markdown
# Config Portal

The device exposes a configuration web portal via SoftAP for first-use setup and on-demand reconfiguration after deployment.

## When the portal opens

1. **First boot:** No WiFi credentials in NVS — portal opens automatically.
2. **GPIO7 button press during deep sleep:** Wakes the device into portal mode (telemetry is skipped for that cycle).

In both cases the device hosts SSID `FireBeetle_C6_Prov` (open WiFi). Connect a phone/laptop and navigate to `http://192.168.4.1`.

The portal auto-exits to deep sleep after 10 minutes idle.

## Pages

- **/wifi** — set WiFi SSID, password, and device ID.
- **/calibrate** — live mV readout; *Capture DRY* (sensor in air) + *Capture WET* (sensor submerged to MAX line) + *Save*.
- **/status** — current stored calibration, last live mV, last percentage, last cal timestamp.
- **/factory-reset** — wipes WiFi credentials *and* calibration; restarts.

## Hardware

- **GPIO7** — momentary push button to GND. Internal pull-up enabled at wake-config time.
- The legacy GPIO20 factory-reset button is removed; factory reset lives in the portal.

## Calibration procedure

1. Press the GPIO7 button. Wait a few seconds for the SoftAP to come up.
2. Connect to `FireBeetle_C6_Prov` and open `http://192.168.4.1/calibrate`.
3. With sensor in dry air, click **Capture DRY**.
4. Submerge sensor to the MAX line, click **Capture WET**.
5. Click **Save & Restart**.

Defaults if calibration is missing in NVS: `dry_mv = 2800`, `wet_mv = 0`. Telemetry still publishes, just less accurate.
```

- [ ] **Step 2: Replace the calibration section of `SOIL_MOISTURE_SETUP.md`**

Open `SOIL_MOISTURE_SETUP.md`. Replace the **entire** "Sensor Calibration" section (everything from `## Sensor Calibration` up to but not including `## How It Works`) with:

```markdown
## Sensor Calibration

Calibration is captured per device at runtime through the config portal. There are no compile-time calibration constants to edit.

See [CONFIG_PORTAL.md](CONFIG_PORTAL.md) for the procedure.
```

- [ ] **Step 3: Delete the retired docs**

```bash
git rm FACTORY_RESET.md WIFI_PROVISIONING.md
```

- [ ] **Step 4: Update `CLAUDE.md`**

In `CLAUDE.md`, in the "Module Responsibilities" table, **replace** the rows for `wifi_provisioning` and `factory_reset` with:

```markdown
| `soil_calibration` | `src/soil_calibration.c` | NVS-backed runtime dry/wet mV values (namespace `soil_cal`) — sane defaults if missing |
| `config_portal` | `src/config_portal.c` | SoftAP (`FireBeetle_C6_Prov`) + HTTP portal (WiFi, calibration, status, factory reset) |
| `form_parser` | `src/form_parser.c` | URL-encoded form field extraction (pure C, host-testable) |
| `nvs_shim` | `src/nvs_shim_esp.c` | u32 wrapper around ESP NVS for host-testable modules |
```

Find the "Soil Moisture Calibration" subsection in `CLAUDE.md` and replace the entire block (including the `#define SENSOR_DRY_MV` / `#define SENSOR_WET_MV` code snippet) with:

```markdown
### Soil Moisture Calibration

Calibration values are stored per device in NVS namespace `soil_cal` (`dry_mv`, `wet_mv`, `cal_ts`) and captured through the config portal. See [CONFIG_PORTAL.md](CONFIG_PORTAL.md). Defaults (`dry_mv=2800`, `wet_mv=0`) are used when NVS has no calibration.
```

In the "Provisioning Flow" section, replace the existing paragraph with:

```markdown
## Provisioning Flow

On first boot (no WiFi credentials in NVS) **or** when the GPIO7 button is pressed during deep sleep, the device opens SoftAP `FireBeetle_C6_Prov` and serves a config portal at `http://192.168.4.1`. See [CONFIG_PORTAL.md](CONFIG_PORTAL.md) for full documentation.
```

- [ ] **Step 5: Update `README.md` references**

Search `README.md` for any mention of "factory reset", "5-second hold", "GPIO 20", or "WIFI_PROVISIONING". Replace with a single sentence pointing to `CONFIG_PORTAL.md`. (If no such references exist, skip this step.)

Run: `grep -nE 'factory.?reset|GPIO ?20|WIFI_PROVISIONING' README.md` to find them.

- [ ] **Step 6: Commit**

```bash
git add CONFIG_PORTAL.md SOIL_MOISTURE_SETUP.md CLAUDE.md README.md
git commit -m "Update docs for config portal and runtime calibration"
```

- [ ] **Step 7: Run the full manual verification checklist from the spec**

Open `docs/superpowers/specs/2026-05-19-soil-moisture-calibration-portal-design.md` and execute every step in the **Manual verification** section. Report any deviation.

- [ ] **Step 8: Final commit if any fix-ups were needed**

```bash
git status
# If anything was modified during manual verification:
git add -A
git commit -m "Manual verification fix-ups"
```

---

## Self-Review

**Spec coverage:**

| Requirement | Covered in |
|---|---|
| R1 (cal persists in NVS) | Tasks 4–6 |
| R2 (`read_raw_mv`) | Task 7 |
| R3 (four portal sections) | Tasks 8–11 |
| R4 (multi-sample capture) | Task 7 (`sample_raw_mv` reused) + Task 10 |
| R5 (GPIO7 wakes into portal) | Task 12 |
| R6 (first-boot includes cal) | Tasks 9 + 10 + 12 (portal also fires on `!is_provisioned`) — note: spec mentions a single submission combining WiFi + calibration; this plan splits it into two pages on a shared portal, which still satisfies "no extra trips" since both are reachable in one session. Capture this clearly in `CONFIG_PORTAL.md` (Task 14). |
| R7 (factory reset wipes both namespaces) | Task 11 |
| R8 (defaults when cal absent) | Tasks 5, 7, 12 |
| N1 (sleep current) | Task 12 (GPIO pull-up only configured at wake-enable time, no `gpio_hold_en` on GPIO7) — verify in spec step 7 |
| N2 (no new deps) | All tasks use existing ESP-IDF + inline snprintf |
| N3 (10-min portal timeout) | Task 8 (`PORTAL_TIMEOUT_SEC = 600`) |
| N4 (telemetry payload unchanged) | `mqtt_publisher_publish_telemetry` signature unchanged across all tasks |
| N5 (test coverage) | Tasks 1, 2, 3, 5, 6 |

**Placeholder scan:** No "TBD" / "implement later" / undefined symbols. All code blocks are complete.

**Type consistency:** `soil_calibration_get_dry_mv` returns `uint32_t` and is cast to `int` for the percentage-math signature — consistent in Tasks 7, 10, 11.

**Note on R6:** The first-boot flow in the spec describes "all four values" submitted together. This plan implements them as two pages (`/wifi` and `/calibrate`) reachable from the same portal, both visited in one session. This is a minor UX deviation but reduces the surface of a single mega-form. Flag this for the user during review of Task 14 docs; revise to a wizard-style single submission if they prefer.

---

## Plan Complete

Plan saved to `docs/superpowers/plans/2026-05-19-soil-moisture-calibration-portal.md`.
