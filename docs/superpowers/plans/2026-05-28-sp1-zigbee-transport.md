# SP1 — Zigbee Transport Bring-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing ESP32-C6 firmware able to report soil moisture + battery as a native Zigbee 3.0 sleepy end-device (behind a `USE_ZIGBEE` build flag), joining the network and reporting via standard ZCL clusters every 15 minutes.

**Architecture:** A new `zigbee_reporter` module owns the ESP Zigbee stack (init, join/restore, cluster registration, attribute reporting, Zigbee-aware deep sleep). A new pure `zigbee_encode` module converts sensor floats to ZCL attribute integers (host-testable). `main.c` gains a `#ifdef USE_ZIGBEE` transport fork. WiFi/MQTT stays the default build; the Zigbee path is a separate PlatformIO env. The sensor/battery/calibration/display modules are reused unchanged.

**Tech Stack:** ESP-IDF C (PlatformIO), ESP Zigbee SDK (`esp-zigbee-lib` + `esp-zboss-lib` managed components), Unity host tests, zigbee2mqtt + a custom external converter.

**Spec:** `docs/superpowers/specs/2026-05-28-sp1-zigbee-transport-design.md`

**SDK reference:** The Zigbee-stack tasks below reference the official `esp-zigbee-sdk` examples. The canonical sleepy-sensor reference is `examples/esp_zigbee_HA_sample/HA_*_sensor` and the deep-sleep reference is `examples/esp_zigbee_sleep`. **Do not invent ESP Zigbee API calls from memory — copy the init/signal-handler/cluster-registration boilerplate from the installed SDK example matching your `esp-zigbee-lib` version, then adapt it to the cluster/attribute/interval requirements spelled out here.**

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `include/zigbee_encode.h` | **Create** | Declarations for the three pure ZCL encoders |
| `src/zigbee_encode.c` | **Create** | Pure float→ZCL-integer encoders (host-testable, no SDK deps) |
| `test/test_zigbee_encode/test_zigbee_encode.c` | **Create** | Unity host tests for the encoders |
| `include/zigbee_reporter.h` | **Create** | Public API: init, report, Zigbee-aware deep sleep |
| `src/zigbee_reporter.c` | **Create** | ESP Zigbee stack lifecycle, endpoint/cluster registration, join/restore, attribute reporting, deep sleep |
| `partitions.csv` | Modify | Add `zb_storage` + `zb_fct` partitions required by the SDK |
| `main/idf_component.yml` | **Create** | Declare `espressif/esp-zigbee-lib` + `espressif/esp-zboss-lib` deps |
| `sdkconfig.defaults.zigbee` | **Create** | Zigbee-specific sdkconfig (radio, ZB enable) for the Zigbee env |
| `platformio.ini` | Modify | New `dfrobot_firebeetle2_esp32c6_zigbee` env with `-DUSE_ZIGBEE`; register encode test |
| `src/CMakeLists.txt` | Modify | Register `zigbee_encode.c` + `zigbee_reporter.c` |
| `src/main.c` | Modify | `#ifdef USE_ZIGBEE` transport fork; Zigbee deep-sleep path |
| `z2m/dfr_soil_moisture.js` | **Create** | zigbee2mqtt external converter (modernExtend) |
| `DEVELOPER_GUIDE.md` | Modify | Zigbee build/flash/pairing + on-device verification notes |

---

## Task 1: Pure ZCL encoders (host TDD)

Fully host-testable. Standard red-green-refactor. No hardware, no SDK.

**Files:**
- Create: `test/test_zigbee_encode/test_zigbee_encode.c`
- Create: `include/zigbee_encode.h`
- Create: `src/zigbee_encode.c`
- Modify: `platformio.ini`

- [ ] **Step 1: Write the failing test**

Create `test/test_zigbee_encode/test_zigbee_encode.c`:

```c
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
```

- [ ] **Step 2: Create the header**

Create `include/zigbee_encode.h`:

```c
#ifndef ZIGBEE_ENCODE_H
#define ZIGBEE_ENCODE_H

#include <stdint.h>

/* Soil moisture percent (0-100) -> ZCL Soil Moisture MeasuredValue.
 * uint16 in 0.01% units, range 0..10000. Clamps out-of-range; NaN/neg -> 0. */
uint16_t zigbee_encode_soil_pct(float pct);

/* Battery volts -> ZCL Power Config BatteryVoltage.
 * uint8 in 100mV units. Caps at 255; NaN/neg -> 0. */
uint8_t zigbee_encode_batt_voltage(float volts);

/* Battery percent (0-100) -> ZCL Power Config BatteryPercentageRemaining.
 * uint8 in 0.5% units, range 0..200. Clamps out-of-range; NaN/neg -> 0. */
uint8_t zigbee_encode_batt_pct(float pct);

#endif // ZIGBEE_ENCODE_H
```

- [ ] **Step 3: Register the test env**

Edit `platformio.ini`. Append `test_zigbee_encode` to the `[env:native]` `test_filter` list so it reads:

```ini
test_filter =
    test_smoke
    test_form_parser
    test_percentage_math
    test_calibration_fallback
    test_display
    test_battery_monitor
    test_zigbee_encode
```

- [ ] **Step 4: Run the test, confirm it fails to compile (no implementation)**

Run: `pio test -e native -f test_zigbee_encode`
Expected: fails — `src/zigbee_encode.c` does not exist yet (file-not-found include error).

- [ ] **Step 5: Write the implementation**

Create `src/zigbee_encode.c`:

```c
#include "zigbee_encode.h"
#include <math.h>

uint16_t zigbee_encode_soil_pct(float pct) {
    if (isnan(pct) || pct <= 0.0f) return 0;
    if (pct >= 100.0f) return 10000;
    return (uint16_t)lroundf(pct * 100.0f);
}

uint8_t zigbee_encode_batt_voltage(float volts) {
    if (isnan(volts) || volts <= 0.0f) return 0;
    long units = lroundf(volts * 10.0f);
    if (units > 255) return 255;
    return (uint8_t)units;
}

uint8_t zigbee_encode_batt_pct(float pct) {
    if (isnan(pct) || pct <= 0.0f) return 0;
    if (pct >= 100.0f) return 200;
    return (uint8_t)lroundf(pct * 2.0f);
}
```

- [ ] **Step 6: Run the test, confirm all pass**

Run: `pio test -e native -f test_zigbee_encode`
Expected: 19 tests pass.

- [ ] **Step 7: Run all native tests (no regressions)**

Run: `pio test -e native`
Expected: all suites pass.

- [ ] **Step 8: Commit**

```bash
git add include/zigbee_encode.h src/zigbee_encode.c test/test_zigbee_encode/test_zigbee_encode.c platformio.ini
git commit -m "zigbee_encode: add host-tested ZCL attribute encoders"
```

---

## Task 2: Build infrastructure for the Zigbee env

Goal: get a **clean compile** of a Zigbee-enabled build env with the ESP Zigbee SDK linked. This validates the highest-friction risk (PlatformIO + ESP component manager) before any application code. No functional behavior yet.

**Files:**
- Modify: `partitions.csv`
- Create: `main/idf_component.yml`
- Create: `sdkconfig.defaults.zigbee`
- Modify: `platformio.ini`

- [ ] **Step 1: Add Zigbee partitions**

Edit `partitions.csv` to append the two SDK-required partitions (they are harmless/unused in the WiFi build, so a single shared table serves both). Final file:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        0x280000,
storage,  data, nvs,     ,        0x4000,
zb_storage, data, fct,   ,        0x4000,
zb_fct,   data, fct,     ,        0x400,
```

> Verify the `Type`/`SubType` of `zb_storage`/`zb_fct` against the `partitions.csv` in your installed `esp-zigbee-sdk` example — if the SDK version uses different values, copy theirs. The C6 has 4 MB flash and the current table uses ~2.7 MB, so the added ~17 KB fits without resizing `factory`.

- [ ] **Step 2: Declare the managed-component dependencies**

Create `main/idf_component.yml`:

```yaml
dependencies:
  espressif/esp-zigbee-lib: "*"
  espressif/esp-zboss-lib: "*"
```

> Pin to specific versions instead of `"*"` if your team prefers reproducible builds — read the latest compatible versions from the ESP Component Registry. `main/idf_component.yml` is the conventional location ESP-IDF's component manager looks for; if PlatformIO's layout puts the primary component elsewhere, place it alongside the `CMakeLists.txt` that registers the app sources (`src/`).

- [ ] **Step 3: Create the Zigbee sdkconfig defaults**

Create `sdkconfig.defaults.zigbee`. Copy the Zigbee-relevant config keys from your installed `esp-zigbee-sdk` example's `sdkconfig.defaults` (these enable the 802.15.4 radio, the Zigbee stack, and partition-table mode). At minimum it must enable the Zigbee end-device role and the IEEE 802.15.4 radio. Representative keys (confirm names against the SDK example for your version):

```
CONFIG_ZB_ENABLED=y
CONFIG_ZB_ZED=y
CONFIG_ZB_RADIO_NATIVE=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

> Do not guess these. Open the SDK example's `sdkconfig.defaults`, take the `CONFIG_ZB_*` and radio lines verbatim. Getting the radio/role config wrong is a common bring-up failure.

- [ ] **Step 4: Add the Zigbee PlatformIO env**

Edit `platformio.ini`. Add a new env that extends the existing board env, enables the flag, and pulls in the Zigbee sdkconfig:

```ini
[env:dfrobot_firebeetle2_esp32c6_zigbee]
extends = env:dfrobot_firebeetle2_esp32c6
build_flags = -DUSE_ZIGBEE
board_build.sdkconfig_defaults = sdkconfig.defaults.zigbee
```

> The exact PlatformIO key for supplying extra sdkconfig defaults can vary by `platform-espressif32` version (`board_build.sdkconfig_defaults` vs a `sdkconfig.defaults.<envname>` filename convention). If the key above is not honored, rename the file to `sdkconfig.defaults.dfrobot_firebeetle2_esp32c6_zigbee` which PlatformIO auto-applies per-env. Verify the flag and config actually take effect in Step 5.

- [ ] **Step 5: Verify the Zigbee env compiles (validates the component-manager risk)**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: the ESP component manager downloads `esp-zigbee-lib` + `esp-zboss-lib`, and the build **succeeds** (it links the Zigbee libs even though no Zigbee code calls them yet — `USE_ZIGBEE` guards are added in later tasks).

- [ ] **Step 6: Verify the default WiFi env still compiles**

Run: `pio run`
Expected: the default `dfrobot_firebeetle2_esp32c6` (WiFi) build still succeeds, unaffected.

- [ ] **Step 7: Commit**

```bash
git add partitions.csv main/idf_component.yml sdkconfig.defaults.zigbee platformio.ini
git commit -m "build: add Zigbee env, partitions, and ESP Zigbee component deps"
```

---

## Task 3: zigbee_reporter skeleton — init, join, signal handler

Bring the stack up and join a network. No reporting yet. Validates that the device commissions and appears in the coordinator.

**Files:**
- Create: `include/zigbee_reporter.h`
- Create: `src/zigbee_reporter.c`
- Modify: `src/CMakeLists.txt`
- Modify: `src/main.c`

- [ ] **Step 1: Create the public header**

Create `include/zigbee_reporter.h`:

```c
#ifndef ZIGBEE_REPORTER_H
#define ZIGBEE_REPORTER_H

#include <stdbool.h>
#include "esp_err.h"

/* Start the Zigbee stack as a sleepy end-device.
 * Restores persisted network state if joined, otherwise begins BDB steering
 * (auto-join into any network with permit-join open).
 * Returns ESP_OK once the stack task is running (join completes asynchronously). */
esp_err_t zigbee_reporter_init(void);

/* Block until the device is on the network or the timeout elapses.
 * Returns true if joined/ready. */
bool zigbee_reporter_wait_ready(uint32_t timeout_ms);

/* Set + report the three sensor attributes to the coordinator.
 * Implemented in Task 4. */
esp_err_t zigbee_reporter_report(float soil_pct, float battery_v, float battery_pct);

#endif // ZIGBEE_REPORTER_H
```

- [ ] **Step 2: Implement stack init + signal handler (adapt from SDK example)**

Create `src/zigbee_reporter.c`. **Start from the `esp_zb_app_signal_handler`, `esp_zb_init`, BDB-steering, and task-creation boilerplate in your installed `esp-zigbee-sdk` HA sensor example**, then adapt to this module's API. The structure must:
- Configure an **end-device** (`ESP_ZB_DEVICE_TYPE_ED`) with sleepy support enabled.
- In the signal handler, on `ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START` / `STEERING`: start steering if not joined; on successful join, set an internal `s_joined = true` flag.
- Provide `zigbee_reporter_init()` that initializes the stack and starts the Zigbee task.
- Provide `zigbee_reporter_wait_ready()` that polls `s_joined` (e.g. a FreeRTOS event group or a simple flag + `vTaskDelay`) up to the timeout.
- Wrap the **entire file body** in `#ifdef USE_ZIGBEE` ... `#endif`. In the WiFi build the file compiles to an empty translation unit — that's fine because `main.c` guards every `zigbee_reporter_*` call behind `#ifdef USE_ZIGBEE` (Step 4), so there are no undefined references. No `#else` stubs are needed. The header (`zigbee_reporter.h`) is still included unconditionally in `main.c`; prototypes-only is harmless in the WiFi build.

Leave `zigbee_reporter_report()` returning `ESP_OK` without doing anything yet (filled in Task 4).

Set the Basic-cluster `ModelIdentifier` to `"DFR-SoilSensor"` (must match the z2m converter in Task 5) and `ManufacturerName` to `"DFRobot-DIY"`.

- [ ] **Step 3: Register the source file**

Edit `src/CMakeLists.txt` — add `"zigbee_encode.c"` and `"zigbee_reporter.c"` to the `SRCS` list (alphabetical placement is fine):

```cmake
        "wifi_credentials.c"
        "wifi_manager.c"
        "zigbee_encode.c"
        "zigbee_reporter.c"
```

- [ ] **Step 4: Add a minimal transport fork in main.c**

Edit `src/main.c`. Add the include near the others:

```c
#include "zigbee_reporter.h"
```

In `app_main()`, after the early OCV sample + brownout gate and **instead of** the WiFi/MQTT path when `USE_ZIGBEE` is defined, branch. Wrap the existing `setup_wifi()`/`setup_mqtt()`/`publish_telemetry_once()` sequence and the new path:

```c
#ifdef USE_ZIGBEE
    if (zigbee_reporter_init() != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee init failed, sleeping");
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;
    }
    if (!zigbee_reporter_wait_ready(30000)) {
        ESP_LOGW(TAG, "Zigbee not ready (not joined?), sleeping");
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;
    }
    ESP_LOGI(TAG, "Zigbee ready (joined)");
    // Reporting wired in Task 4; sleep model finalized in Task 6.
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
    return;
#else
    // existing WiFi path: setup_wifi() -> setup_mqtt() -> publish_telemetry_once()
    ...
#endif
```

Keep the existing WiFi code exactly as-is inside the `#else`.

- [ ] **Step 5: Build both envs**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: success.
Run: `pio run`
Expected: success (WiFi path unchanged).

- [ ] **Step 6: On-device — verify join**

Flash the Zigbee env: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor`
In zigbee2mqtt, enable `permit_join`. Power-cycle the device.
Expected: serial log shows BDB steering + a successful join signal; the device appears in zigbee2mqtt's device list (likely as an "unsupported"/interview-incomplete device — that's fine, the converter comes in Task 5).

- [ ] **Step 7: Commit**

```bash
git add include/zigbee_reporter.h src/zigbee_reporter.c src/CMakeLists.txt src/main.c
git commit -m "zigbee_reporter: stack init + BDB join; main.c transport fork skeleton"
```

---

## Task 4: Cluster/endpoint registration + attribute reporting

Register the four clusters and report real encoded sensor values.

**Files:**
- Modify: `src/zigbee_reporter.c`
- Modify: `src/main.c`

- [ ] **Step 1: Register clusters on the endpoint (adapt from SDK example)**

In `src/zigbee_reporter.c`, during stack init, build an endpoint with these server clusters (use the SDK's `esp_zb_*_cluster_create` / `esp_zb_cluster_list` / `esp_zb_ep_list` helpers — copy the pattern from the SDK sensor example):

| Cluster | ID | Notes |
|---|---|---|
| Basic | 0x0000 | ManufacturerName `"DFRobot-DIY"`, ModelIdentifier `"DFR-SoilSensor"`, PowerSource = battery |
| Identify | 0x0003 | default |
| Power Configuration | 0x0001 | attributes BatteryVoltage (0x0020, uint8), BatteryPercentageRemaining (0x0021, uint8) |
| Soil Moisture Measurement | 0x0408 | MeasuredValue (0x0000, uint16), MinMeasuredValue (0x0001)=0, MaxMeasuredValue (0x0002)=10000 |

- [ ] **Step 2: Implement `zigbee_reporter_report()`**

Fill in the body using the encoders from Task 1. Pseudocode (use the SDK's `esp_zb_zcl_set_attribute_val` + report APIs for the real calls):

```c
#include "zigbee_encode.h"

esp_err_t zigbee_reporter_report(float soil_pct, float battery_v, float battery_pct) {
    uint16_t soil = zigbee_encode_soil_pct(soil_pct);
    uint8_t  volt = zigbee_encode_batt_voltage(battery_v);
    uint8_t  pct  = zigbee_encode_batt_pct(battery_pct);

    // esp_zb_lock_acquire(...)
    // esp_zb_zcl_set_attribute_val(endpoint, 0x0408, SERVER, 0x0000, &soil, false);
    // esp_zb_zcl_set_attribute_val(endpoint, 0x0001, SERVER, 0x0020, &volt, false);
    // esp_zb_zcl_set_attribute_val(endpoint, 0x0001, SERVER, 0x0021, &pct,  false);
    // trigger report (esp_zb_zcl_report_attr_cmd_req for each, or rely on reporting config)
    // esp_zb_lock_release(...)
    ESP_LOGI("ZB_RPT", "report soil=%u volt=%u pct=%u", soil, volt, pct);
    return ESP_OK;
}
```

Use the exact SDK attribute-set/report API names from the example. The encoder calls and the log line are the parts fixed by this plan.

- [ ] **Step 3: Wire real sensor reads in main.c**

In the `#ifdef USE_ZIGBEE` block of `app_main()`, replace the placeholder between "Zigbee ready" and sleep with real reads (mirroring the WiFi path's `publish_telemetry_once`):

```c
    float soil = soil_moisture_read_percentage();
    float batt_pct = battery_monitor_v_to_pct(g_cached_battery_v);
    ESP_LOGI(TAG, "Zigbee report: soil=%.1f%% batt=%.2fV (%.0f%%)",
             soil, g_cached_battery_v, batt_pct);
    zigbee_reporter_report(soil, g_cached_battery_v, batt_pct);
    vTaskDelay(pdMS_TO_TICKS(PUBLISH_WAIT_MS));  // drain TX before sleep
```

(`g_cached_battery_v` is the OCV captured by the existing early-sample block.)

- [ ] **Step 4: Build both envs**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee` → success.
Run: `pio run` → success.

- [ ] **Step 5: On-device — verify raw attribute reporting**

Flash + monitor the Zigbee env. With the device joined, watch zigbee2mqtt's debug log / the device's exposed attributes.
Expected: the Soil Moisture (0x0408) MeasuredValue and Power Config battery attributes appear with plausible values (probe in air → low soil %, battery near 4.2 V). They may show as raw cluster values until the converter (Task 5) is added.

- [ ] **Step 6: Commit**

```bash
git add src/zigbee_reporter.c src/main.c
git commit -m "zigbee_reporter: register clusters and report encoded soil + battery"
```

---

## Task 5: zigbee2mqtt external converter

Make the device present clean `soil_moisture` and `battery` entities in zigbee2mqtt.

**Files:**
- Create: `z2m/dfr_soil_moisture.js`

- [ ] **Step 1: Write the converter**

Create `z2m/dfr_soil_moisture.js`:

```javascript
const {soilMoisture, battery, identify} = require('zigbee-herdsman-converters/lib/modernExtend');

module.exports = [
    {
        zigbeeModel: ['DFR-SoilSensor'],
        model: 'DFR-SoilSensor',
        vendor: 'DFRobot-DIY',
        description: 'ESP32-C6 soil moisture + battery sensor (DIY)',
        extend: [
            soilMoisture(),
            battery(),
            identify(),
        ],
    },
];
```

> Verify the `modernExtend` export names (`soilMoisture`, `battery`, `identify`) against the zigbee-herdsman-converters version installed in your zigbee2mqtt. If `soilMoisture()` is not exported in your version, fall back to a `fromZigbee` converter on cluster `msSoilMoisture` attribute `measuredValue` dividing by 100. The `zigbeeModel` string MUST equal the Basic-cluster ModelIdentifier set in Task 3/4 (`DFR-SoilSensor`).

- [ ] **Step 2: Install the converter in zigbee2mqtt**

Copy `z2m/dfr_soil_moisture.js` to the zigbee2mqtt config directory and reference it under `external_converters:` in zigbee2mqtt's `configuration.yaml` (or the newer external-extension mechanism for your z2m version). Restart zigbee2mqtt.

- [ ] **Step 3: On-device — verify clean entities**

Re-interview the device in zigbee2mqtt (or remove + rejoin).
Expected: the device shows `soil_moisture` (%) and `battery` (%) — and `voltage` if your `battery()` extend exposes it — as proper entities, tracking real readings.

- [ ] **Step 4: Commit**

```bash
git add z2m/dfr_soil_moisture.js
git commit -m "z2m: external converter for DFR-SoilSensor (modernExtend)"
```

---

## Task 6: Deep-sleep + persisted-state report loop

Add the real power model: deep sleep between reports, restore network state on wake, report within the 32-min end-device timeout (no rejoin). Validates the top two spec risks.

**Files:**
- Modify: `src/zigbee_reporter.c`
- Modify: `src/main.c`

- [ ] **Step 1: Configure sleepy end-device + end-device timeout (adapt from SDK sleep example)**

In `src/zigbee_reporter.c`, enable Zigbee deep-sleep support and request a long end-device timeout. **Copy the deep-sleep enable + power-management pattern from the `esp_zigbee_sleep` SDK example.** Requirements fixed by this plan:
- Enable Zigbee sleep (`esp_zb_sleep_enable(true)` + the SDK's deep-sleep configuration).
- Request an end-device timeout of **32 minutes** (the nearest enum ≥ 2× the 15-min report interval) via the SDK's end-device-timeout API or `aging timeout` config.
- Ensure network parameters are persisted to the `zb_storage` NVS partition across deep sleep (this is the SDK default when NVRAM is configured; confirm).

- [ ] **Step 2: Add the report interval constant + Zigbee deep-sleep entry in main.c**

Edit `src/main.c`. Add near the other config constants:

```c
#define ZIGBEE_REPORT_INTERVAL_SEC  900   // 15 minutes (< 32-min end-device timeout)
```

In the `#ifdef USE_ZIGBEE` block, change the post-report sleep from `DEEP_SLEEP_INTERVAL_SEC` to `ZIGBEE_REPORT_INTERVAL_SEC`:

```c
    enter_deep_sleep(ZIGBEE_REPORT_INTERVAL_SEC);
```

`enter_deep_sleep()` already configures a timer wakeup and isolates analog pins — reuse it. (If the SDK requires a specific deep-sleep entry call to persist Zigbee NVRAM first, invoke that in `zigbee_reporter` and call it from the Zigbee branch *before* `enter_deep_sleep()`; expose it as `zigbee_reporter_prepare_sleep()` if needed.)

- [ ] **Step 3: Build both envs**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee` → success.
Run: `pio run` → success.

- [ ] **Step 4: On-device — verify rejoin-free sleep cycles (Risk 1 + 2)**

Flash + monitor. Let the device run through **at least 3 consecutive wake→report→sleep cycles** (~45 min, or temporarily lower `ZIGBEE_REPORT_INTERVAL_SEC` to e.g. 120 s for faster iteration — remember to restore it).
Expected:
- After the first join, subsequent wakes report **without a new join** appearing in zigbee2mqtt's log (no join-spam).
- `soil_moisture` / `battery` values update each cycle.
- If the coordinator ages the device out (rejoin every cycle), note it — the fallback still works but Risk 2 materialized; record the observed end-device-timeout behavior for the spec's "known limitations."

- [ ] **Step 5: Commit**

```bash
git add src/zigbee_reporter.c src/main.c
git commit -m "zigbee_reporter: sleepy end-device deep sleep with 15-min report interval"
```

---

## Task 7: Finalize main.c transport fork (brownout gate + display)

Tighten the integration so the Zigbee path mirrors the WiFi path's brownout gate and display refresh.

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Confirm the brownout gate precedes the Zigbee path**

Verify in `app_main()` that the early OCV sample + `battery_monitor_is_safe()` gate runs **before** the `#ifdef USE_ZIGBEE` branch (it should already, from the existing structure). If the cell is below `BATTERY_LOW_CUTOFF_V`, the device must skip Zigbee init entirely, show the low-battery screen (one-shot, RTC-latched), and sleep — identical to the WiFi path. No code change if the gate already sits above the fork; otherwise move the fork below the gate.

- [ ] **Step 2: Add display refresh to the Zigbee path**

In the `#ifdef USE_ZIGBEE` block, after a successful report and before sleep, refresh the e-paper exactly as the WiFi path does:

```c
    display_telemetry_t dt = {
        .device_id     = device_id_buffer,
        .moisture_pct  = soil,
        .raw_mv        = soil_moisture_read_raw_mv(),
        .battery_v     = g_cached_battery_v,
        .battery_pct   = display_battery_v_to_pct(g_cached_battery_v),
        .wifi_rssi_dbm = 0,   // no WiFi RSSI in Zigbee mode
    };
    if (display_init() == ESP_OK) {
        display_show_telemetry(&dt);
        display_deinit();
    }
```

> `device_id_buffer` is currently populated in `setup_mqtt()` (WiFi path). For the Zigbee build, populate it from `wifi_credentials_load_device_id()` (NVS) or the `DEFAULT_DEVICE_ID` fallback near the top of the Zigbee branch, since `setup_mqtt()` isn't called. Reuse the existing load logic.

- [ ] **Step 3: Build both envs**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee` → success.
Run: `pio run` → success.

- [ ] **Step 4: On-device — verify display + brownout**

Flash + monitor. Confirm the e-paper shows current soil/battery after a Zigbee report. If a bench supply is available, drop below 3.70 V and confirm the low-battery screen appears and Zigbee init is skipped (same as WiFi behavior). If no bench supply, note this sub-check as deferred.

- [ ] **Step 5: Commit**

```bash
git add src/main.c
git commit -m "main: Zigbee path honors brownout gate and refreshes e-paper"
```

---

## Task 8: Documentation

**Files:**
- Modify: `DEVELOPER_GUIDE.md`

- [ ] **Step 1: Add a Zigbee build/flash/verify section**

Append a `## Zigbee Build (SP1)` section to `DEVELOPER_GUIDE.md` documenting:

```markdown
## Zigbee Build (SP1)

The Zigbee transport is behind a build flag. WiFi/MQTT remains the default.

### Build & flash the Zigbee firmware
```
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor
```

### Pairing
1. In zigbee2mqtt, enable `permit_join`.
2. Power-cycle the sensor (or first flash). It auto-runs BDB steering and joins.
3. Install `z2m/dfr_soil_moisture.js` as an external converter and restart zigbee2mqtt.
4. The device appears as `DFR-SoilSensor` with `soil_moisture` and `battery` entities.

### Verification checklist
- [ ] Device joins and appears in zigbee2mqtt without manual interview errors.
- [ ] `soil_moisture` tracks reality (low in air, high in water).
- [ ] `battery` % and voltage are plausible (note: inflated while solar-charging — known limitation).
- [ ] ≥3 consecutive 15-min cycles report without a new join logged (rejoin-free; confirms the 32-min end-device timeout holds on this coordinator).
- [ ] e-paper shows current readings after each report.

### Known limitations (SP1)
- Battery SoC reads high during daylight solar charging (terminal sits at charger CV voltage).
- Commissioning is auto-steer-on-boot only; button-driven pairing arrives in SP3.
```

- [ ] **Step 2: Commit**

```bash
git add DEVELOPER_GUIDE.md
git commit -m "docs: Zigbee build, pairing, and verification notes (SP1)"
```

---

## Final Verification

- [ ] **All host tests pass**

Run: `pio test -e native`
Expected: all suites pass, including `test_zigbee_encode`.

- [ ] **Both firmware envs build cleanly**

Run: `pio run` (WiFi default) → success.
Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee` → success.

- [ ] **On-device end-to-end**

Zigbee firmware joins, reports soil + battery via standard clusters, shows clean entities in zigbee2mqtt through the external converter, survives ≥3 rejoin-free sleep cycles, and refreshes the e-paper. Complete the DEVELOPER_GUIDE verification checklist.

- [ ] **Risk notes captured**

Record observed behavior for the three hardware risks (SDK deep-sleep retention, coordinator end-device-timeout, PlatformIO component manager) in the spec's "known limitations" or a follow-up note, so SP3 planning has real data.
