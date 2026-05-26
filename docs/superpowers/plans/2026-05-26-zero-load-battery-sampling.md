# Zero-Load Battery Sampling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sample battery OCV before WiFi energizes, gate the publish cycle on a 3.70V safety threshold with an e-paper warning, and replace the linear SoC ramp with an 11-point LiPo discharge curve as the single source of truth.

**Architecture:** Pure SoC + safety helpers live in `battery_monitor.c` behind a new host-safe header `battery_soc.h` so they can be reused by `display.c` without dragging in ESP-IDF. The existing hardware code (`adc_oneshot`, calibration) is gated by `TEST_HOST` so tests can include the source directly. `main.c` adds an early-sample block after the portal-mode check and before WiFi setup, with an RTC-memory latch to ensure the e-paper warning refreshes at most once per low-battery episode.

**Tech Stack:** ESP-IDF C (PlatformIO), Unity test framework on the native host env, RTC memory for cross-sleep state.

**Spec:** `docs/superpowers/specs/2026-05-26-zero-load-battery-sampling-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `include/battery_soc.h` | **Create** | Host-safe header: `BATTERY_LOW_CUTOFF_V`, `battery_monitor_v_to_pct`, `battery_monitor_is_safe` |
| `include/battery_monitor.h` | Modify | Unchanged externally; will be paired with new `battery_soc.h` for callers |
| `src/battery_monitor.c` | Modify | Add SoC LUT + pure helpers above a new `#ifndef TEST_HOST` guard wrapping the existing hardware code |
| `include/display.h` | Modify | Add `display_show_low_battery(float volts)` declaration |
| `src/display.c` | Modify | Reduce `display_battery_v_to_pct` to a wrapper; implement `display_show_low_battery` |
| `src/main.c` | Modify | RTC latch, early-sample block, cached voltage in `publish_telemetry_once` |
| `src/CMakeLists.txt` | Unchanged | No new `.c` files; only header added |
| `test/test_battery_monitor/test_battery_monitor.c` | **Create** | Unity tests for `v_to_pct` and `is_safe` |
| `platformio.ini` | Modify | Add `test_battery_monitor` to native env `test_filter` |
| `test/test_display/test_display.c` | Modify | Update `display_battery_v_to_pct` expected values for new curve |
| `DEVELOPER_GUIDE.md` | Modify | Append bench-test checklist entries |

---

## Task 1: Gate existing battery_monitor.c hardware code behind TEST_HOST

This is purely structural — wrapping existing code in `#ifndef TEST_HOST` so subsequent tasks can include the source file in a host test.

**Files:**
- Modify: `src/battery_monitor.c`

- [ ] **Step 1: Read the current file to confirm structure**

Run: `wc -l src/battery_monitor.c` — expect ~123 lines.

- [ ] **Step 2: Wrap the file body in a TEST_HOST guard**

Edit `src/battery_monitor.c`. After the `#include` block and before the first `static const char *TAG`, add:

```c
#ifndef TEST_HOST
```

At the very end of the file (after the closing `}` of `battery_monitor_deinit`), add:

```c
#endif // TEST_HOST
```

Also wrap the includes that pull in ESP-IDF:

```c
#include "battery_monitor.h"
#ifndef TEST_HOST
#include "adc_manager.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"
#endif
```

- [ ] **Step 3: Verify the firmware build still succeeds**

Run: `pio run`
Expected: build succeeds with no battery_monitor warnings.

- [ ] **Step 4: Commit**

```bash
git add src/battery_monitor.c
git commit -m "battery_monitor: gate hardware code behind TEST_HOST guard"
```

---

## Task 2: Create host-safe battery_soc.h header

**Files:**
- Create: `include/battery_soc.h`

- [ ] **Step 1: Write the header**

Create `include/battery_soc.h` with the following exact contents:

```c
#ifndef BATTERY_SOC_H
#define BATTERY_SOC_H

#include <stdbool.h>

/* Cell voltage at or above which the device is allowed to power up
 * WiFi / MQTT for a normal publish cycle. Below this, the firmware
 * skips the cycle and goes back to deep sleep. Chosen to protect the
 * LiPo cell from over-discharge and to match the 20% SoC knee. */
#define BATTERY_LOW_CUTOFF_V  3.70f

/* Convert cell voltage to state-of-charge percentage (0.0–100.0)
 * via piecewise-linear interpolation over an 11-point LiPo curve.
 * Clamps at and below 3.20V to 0.0, at and above 4.20V to 100.0.
 * Defensive: NaN or negative input returns 0.0.
 * Pure function — host-testable, no ESP-IDF dependencies. */
float battery_monitor_v_to_pct(float volts);

/* Returns true iff volts >= BATTERY_LOW_CUTOFF_V.
 * Boundary is inclusive: exactly 3.70V is considered safe. */
bool battery_monitor_is_safe(float volts);

#endif // BATTERY_SOC_H
```

- [ ] **Step 2: Verify the firmware build still succeeds**

Run: `pio run`
Expected: build succeeds (no callers yet, just header presence).

- [ ] **Step 3: Commit**

```bash
git add include/battery_soc.h
git commit -m "battery_soc: add host-safe header for pure helpers"
```

---

## Task 3: TDD — failing tests for battery_monitor_v_to_pct and is_safe

**Files:**
- Create: `test/test_battery_monitor/test_battery_monitor.c`
- Modify: `platformio.ini`

- [ ] **Step 1: Create the test file**

Create `test/test_battery_monitor/test_battery_monitor.c`:

```c
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
```

- [ ] **Step 2: Register the new test directory with the native env**

Edit `platformio.ini`. In the `[env:native]` block, append `test_battery_monitor` to the `test_filter` list. The block should end up looking like:

```ini
[env:native]
platform = native
framework =
test_framework = unity
build_flags = -std=c11 -Wall -Wextra -I include
build_src_filter = -<*>
test_filter =
    test_smoke
    test_form_parser
    test_percentage_math
    test_calibration_fallback
    test_display
    test_battery_monitor
```

- [ ] **Step 3: Run the test, confirm it fails to compile (function not defined)**

Run: `pio test -e native -f test_battery_monitor`
Expected: build fails with `implicit declaration of function 'battery_monitor_v_to_pct'` or `'battery_monitor_is_safe'`.

- [ ] **Step 4: Commit the failing test**

```bash
git add test/test_battery_monitor/test_battery_monitor.c platformio.ini
git commit -m "test: add failing tests for battery SoC curve and safety check"
```

---

## Task 4: Implement battery_monitor_v_to_pct and is_safe

**Files:**
- Modify: `src/battery_monitor.c`

- [ ] **Step 1: Add includes for the pure section**

At the top of `src/battery_monitor.c`, immediately after `#include "battery_monitor.h"` and **before** the `#ifndef TEST_HOST` guard added in Task 1, add:

```c
#include "battery_soc.h"
#include <math.h>
#include <stddef.h>
```

- [ ] **Step 2: Add the pure implementation above the TEST_HOST guard**

Still in `src/battery_monitor.c`, after the includes but **before** `#ifndef TEST_HOST`, insert:

```c
// ============================================================================
// Pure helpers (host-testable)
// ============================================================================

typedef struct { float v; float pct; } soc_point_t;

// 11-point LiPo discharge curve, descending by voltage.
static const soc_point_t SOC_LUT[] = {
    {4.20f, 100.0f},
    {4.05f,  90.0f},
    {3.96f,  80.0f},
    {3.90f,  70.0f},
    {3.85f,  60.0f},
    {3.80f,  50.0f},
    {3.76f,  40.0f},
    {3.73f,  30.0f},
    {3.70f,  20.0f},
    {3.65f,  10.0f},
    {3.20f,   0.0f},
};
static const size_t SOC_LUT_N = sizeof(SOC_LUT) / sizeof(SOC_LUT[0]);

float battery_monitor_v_to_pct(float volts) {
    // Defensive: NaN or negative -> 0%
    if (isnan(volts) || volts <= 0.0f) return 0.0f;
    // Clamp above max
    if (volts >= SOC_LUT[0].v) return SOC_LUT[0].pct;
    // Clamp below min
    if (volts <= SOC_LUT[SOC_LUT_N - 1].v) return SOC_LUT[SOC_LUT_N - 1].pct;
    // Find the bracketing pair (table is descending in voltage)
    for (size_t i = 0; i < SOC_LUT_N - 1; i++) {
        float v_hi = SOC_LUT[i].v;
        float v_lo = SOC_LUT[i + 1].v;
        if (volts <= v_hi && volts >= v_lo) {
            float pct_hi = SOC_LUT[i].pct;
            float pct_lo = SOC_LUT[i + 1].pct;
            float frac = (volts - v_lo) / (v_hi - v_lo);
            return pct_lo + frac * (pct_hi - pct_lo);
        }
    }
    return 0.0f;  // unreachable given the clamps above
}

bool battery_monitor_is_safe(float volts) {
    return volts >= BATTERY_LOW_CUTOFF_V;
}
```

- [ ] **Step 3: Run the tests, confirm all pass**

Run: `pio test -e native -f test_battery_monitor`
Expected: all 21 tests pass.

- [ ] **Step 4: Verify the firmware build still succeeds**

Run: `pio run`
Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add src/battery_monitor.c
git commit -m "battery_monitor: implement 11-point SoC curve and 3.70V safety check"
```

---

## Task 5: Update display_battery_v_to_pct to wrap the new curve

The existing display test asserts the old linear curve (e.g. 3.75V → ~50%). Under the new curve, 3.75V interpolates between 3.76V/40% and 3.73V/30% → ~36.7% → rounds to 37%. Tests need updating.

**Files:**
- Modify: `src/display.c`
- Modify: `test/test_display/test_display.c`

- [ ] **Step 1: Update display.c — include battery_soc.h and rewrite the wrapper**

In `src/display.c`, near the top (with the other includes that are above the `#ifndef TEST_HOST` guard), add:

```c
#include "battery_soc.h"
```

(This header is host-safe, so it can be unconditionally included regardless of TEST_HOST.)

Then replace the existing `display_battery_v_to_pct` function (currently around lines 29–37) with:

```c
int display_battery_v_to_pct(float v) {
    float pct = battery_monitor_v_to_pct(v);
    int i = (int)(pct + 0.5f);
    if (i < 0) return 0;
    if (i > 100) return 100;
    return i;
}
```

- [ ] **Step 2: Update test_display.c to include battery_monitor.c source under TEST_HOST**

The display test includes `display.c` under `TEST_HOST`. Now that `display.c` references `battery_monitor_v_to_pct`, the test also needs the SUT symbol available. In `test/test_display/test_display.c`, immediately after the existing `#include "../../src/display.c"` line, add:

```c
#include "../../src/battery_monitor.c"
```

(Both files have a `TEST_HOST` guard around their hardware code, so including the source files is safe.)

- [ ] **Step 3: Update the curve-shape assertions in test_display.c**

Replace the four affected tests in `test/test_display/test_display.c` with values that match the new piecewise curve:

```c
static void test_battery_near_floor_is_low(void) {
    // 3.30V is on the long flat tail between 3.65V/10% and 3.20V/0%.
    // Linear interp: (3.30-3.20)/(3.65-3.20) * 10% = ~2.22% -> rounds to 2.
    int p = display_battery_v_to_pct(3.30f);
    TEST_ASSERT_TRUE_MESSAGE(p >= 0 && p <= 3, "3.30V should be very low");
}

static void test_battery_at_ceiling_is_hundred(void) {
    TEST_ASSERT_EQUAL_INT(100, display_battery_v_to_pct(4.2f));
}

static void test_battery_midpoint(void) {
    // 3.80V is an exact LUT point at 50%.
    TEST_ASSERT_EQUAL_INT(50, display_battery_v_to_pct(3.80f));
}

static void test_battery_below_floor_clamps_to_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, display_battery_v_to_pct(2.5f));
}

static void test_battery_above_ceiling_clamps_to_hundred(void) {
    TEST_ASSERT_EQUAL_INT(100, display_battery_v_to_pct(4.5f));
}
```

- [ ] **Step 4: Update the RUN_TEST registration for the renamed test**

In the same file, in `main()`, change:

```c
RUN_TEST(test_battery_at_floor_is_zero);
```

to:

```c
RUN_TEST(test_battery_near_floor_is_low);
```

The other four RUN_TEST lines (ceiling, midpoint, below_floor_clamps_to_zero, above_ceiling_clamps_to_hundred) keep their existing names.

- [ ] **Step 5: Run the display tests, confirm they pass**

Run: `pio test -e native -f test_display`
Expected: all tests pass.

- [ ] **Step 6: Run all native tests to confirm no regressions**

Run: `pio test -e native`
Expected: all tests pass.

- [ ] **Step 7: Verify firmware build**

Run: `pio run`
Expected: build succeeds.

- [ ] **Step 8: Commit**

```bash
git add src/display.c test/test_display/test_display.c
git commit -m "display: route battery_v_to_pct through battery_monitor curve"
```

---

## Task 6: Add display_show_low_battery API

No host test for the rendering (matches the existing pattern — `display_show_portal` and `display_show_telemetry` aren't host-tested either). Verification is manual via the bench-test step at the end.

**Files:**
- Modify: `include/display.h`
- Modify: `src/display.c`

- [ ] **Step 1: Add the declaration to display.h**

Insert the following into `include/display.h`, immediately after the existing `void display_show_portal(void);` declaration:

```c
/* Render a minimal full-screen "LOW BATTERY <volts>V" warning.
 * Intended for one-shot display when battery_monitor_is_safe() is false.
 * Uses large text only — fast refresh, minimal energy. */
void display_show_low_battery(float volts);
```

- [ ] **Step 2: Implement display_show_low_battery in display.c**

Append the following inside the `#ifndef TEST_HOST` section of `src/display.c`, just before `void display_deinit(void)`:

```c
void display_show_low_battery(float volts) {
    fb_clear(0xFF);  // white background

    // Header (matches the portal layout style).
    draw_text_small_2x_centered(0, DISPLAY_W, 6, "LOW");
    draw_text_small_2x_centered(0, DISPLAY_W, 28, "BATTERY");

    // Voltage line, e.g. "3.65 V"
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%.2f V", (double)volts);
    draw_text_small_2x_centered(0, DISPLAY_W, 90, vbuf);

    // Instruction line at the bottom.
    draw_text_small_centered(0, DISPLAY_W, DISPLAY_H_PX - 24, "CHARGE TO RESUME");

    panel_refresh_full();
}
```

- [ ] **Step 3: Verify the firmware build succeeds**

Run: `pio run`
Expected: build succeeds.

- [ ] **Step 4: Run host tests to confirm display.c still compiles under TEST_HOST**

Run: `pio test -e native`
Expected: all tests pass (the new function lives inside the `#ifndef TEST_HOST` block, so it's not in the host build).

- [ ] **Step 5: Commit**

```bash
git add include/display.h src/display.c
git commit -m "display: add low-battery warning screen"
```

---

## Task 7: Wire main.c — RTC latch, early sample, cached voltage

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Add includes and file-static state**

In `src/main.c`, add to the `#include` block (near the other module includes):

```c
#include "battery_soc.h"
```

Then add the following file-static state, near the existing `static char mqtt_topic_buffer[...]` declarations (around line 58):

```c
// Cached OCV captured at the very top of app_main(), reused by publish_telemetry_once().
static float g_cached_battery_v = 0.0f;

// Persists across deep sleep: latches when the low-battery warning has been drawn,
// so we don't burn ~30 mJ refreshing the e-paper every hour while the cell is starved.
RTC_DATA_ATTR static bool s_low_battery_shown = false;
```

- [ ] **Step 2: Add the early-sample block in app_main()**

In `src/main.c`, locate the existing portal-mode check (around lines 514–518):

```c
if (wake_cause == ESP_SLEEP_WAKEUP_GPIO || !wifi_credentials_is_provisioned()) {
    run_portal_then_sleep();
    return;
}
```

Immediately **after** that block (and before `setup_wifi()`), insert:

```c
// ------------------------------------------------------------------
// Zero-load battery sample: must happen before WiFi/MQTT energize.
// init_system() only touches NVS, event loop, and ADC — no radio yet.
// ------------------------------------------------------------------
float ocv = battery_monitor_read_voltage();
ESP_LOGI(TAG, "OCV: %.3fV (%.0f%% SoC)", ocv, battery_monitor_v_to_pct(ocv));

if (!battery_monitor_is_safe(ocv)) {
    ESP_LOGE(TAG, "*** LOW BATTERY %.2fV < %.2fV - skipping WiFi/MQTT ***",
             ocv, BATTERY_LOW_CUTOFF_V);
    if (!s_low_battery_shown) {
        s_low_battery_shown = true;     // latch first, refresh second
        if (display_init() == ESP_OK) {
            display_show_low_battery(ocv);
            display_deinit();
        }
    }
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
    return;
}
s_low_battery_shown = false;            // healthy reading clears the latch
g_cached_battery_v = ocv;               // reused by publish_telemetry_once()
```

- [ ] **Step 3: Change publish_telemetry_once to use the cached voltage**

In `src/main.c`, in `publish_telemetry_once()` (around line 393), replace:

```c
// Read battery voltage
float voltage = battery_monitor_read_voltage();
```

with:

```c
// Battery voltage was captured at the top of app_main() before WiFi powered up,
// so it reflects open-circuit voltage rather than the sagging-under-load value.
float voltage = g_cached_battery_v;
```

- [ ] **Step 4: Verify the firmware build succeeds**

Run: `pio run`
Expected: build succeeds.

- [ ] **Step 5: Run all host tests for regression**

Run: `pio test -e native`
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/main.c
git commit -m "main: zero-load battery sample with brownout gate and one-shot warning"
```

---

## Task 8: Document manual bench verification

**Files:**
- Modify: `DEVELOPER_GUIDE.md`

- [ ] **Step 1: Check whether DEVELOPER_GUIDE.md exists and has a validation checklist**

Run: `grep -n "Validation\|Checklist\|Manual" DEVELOPER_GUIDE.md | head -10`
Expected: at least one heading like "Validation" or "Manual Verification". If none, append a new `## Manual Verification` section.

- [ ] **Step 2: Append battery-sampling checklist entries**

Append the following block to `DEVELOPER_GUIDE.md` under the most appropriate validation/checklist section (or under a new `## Battery Sampling — Bench Verification` section if none fits):

```markdown
### Battery Sampling — Bench Verification

After flashing changes to battery_monitor / main, verify on the bench supply:

- [ ] Serial log shows an `OCV: x.xxxV (y% SoC)` line **before** the first WiFi log line on a timer wake.
- [ ] With bench supply at 3.50 V at the battery input, the device:
  - Logs `*** LOW BATTERY 3.50V < 3.70V - skipping WiFi/MQTT ***`
  - Refreshes the e-paper to show `LOW BATTERY 3.50 V`
  - Returns to deep sleep without any WiFi traffic
- [ ] On the next wake at 3.50 V, the device skips the redraw (RTC latch held); only the log line appears.
- [ ] Raise the bench supply to 3.80 V before the next wake: the device resumes a normal publish, the latch clears, and a subsequent forced drop below 3.70 V re-draws the warning.
- [ ] On a healthy publish, the MQTT-published `battery_v` is meaningfully higher than the previous (under-load) reading at the same actual cell voltage. (Compare against a known telemetry sample from before this change at similar SoC.)
- [ ] Pressing the GPIO7 button at low voltage still opens the config portal (battery gate is bypassed for portal wakes).
```

- [ ] **Step 3: Commit**

```bash
git add DEVELOPER_GUIDE.md
git commit -m "docs: add bench-verification checklist for zero-load battery sampling"
```

---

## Final Verification

- [ ] **All host tests pass**

Run: `pio test -e native`
Expected: all tests pass across `test_smoke`, `test_form_parser`, `test_percentage_math`, `test_calibration_fallback`, `test_display`, `test_battery_monitor`.

- [ ] **Firmware builds cleanly**

Run: `pio run`
Expected: build succeeds with no new warnings.

- [ ] **Flash and complete the bench-verification checklist in DEVELOPER_GUIDE.md.**
