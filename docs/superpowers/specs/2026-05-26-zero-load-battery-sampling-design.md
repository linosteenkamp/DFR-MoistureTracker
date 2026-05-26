# Zero-Load Battery Sampling & Brownout Protection

**Date:** 2026-05-26
**Status:** Approved (pre-implementation)
**Scope:** `src/battery_monitor.{c,h}`, `src/display.{c,h}`, `src/main.c`, host tests

## Problem

Battery voltage is currently sampled inside `publish_telemetry_once()`, *after* WiFi and MQTT are connected and drawing ~85 mA. The 2:1 voltage divider feeding GPIO 0 sees the sagging cell voltage, not the true Open-Circuit Voltage (OCV), so the reported `battery_v` and any derived state-of-charge are systematically low and noisy.

Three related gaps:

1. **No OCV measurement.** Reading happens under load, not before it.
2. **No SoC curve.** `display_battery_v_to_pct()` is a linear ramp from 3.30V→0% to 4.20V→100% — bears no relation to the LiPo discharge curve, which has a knee around 3.7V.
3. **No brownout protection.** If the cell drops below safe-discharge voltage, the firmware still powers up WiFi, which both wastes the remaining charge and risks permanent LiPo degradation.

## Goals

- Capture true OCV by sampling at the start of `app_main()`, before any radio or high-load peripheral is energized.
- Translate cell voltage to SoC% via an 11-point piecewise-linear LiPo discharge curve.
- Skip the publish cycle entirely when OCV < 3.60 V and surface a visible warning on the e-paper.
- Single source of truth for the SoC curve.

## Non-Goals

- Replacing the existing ESP-IDF `adc_oneshot` + factory-calibration path. It is already the correct equivalent of Arduino's `analogReadMilliVolts()` and is kept as-is.
- Changing the deep-sleep interval (`DEEP_SLEEP_INTERVAL_SEC` stays at 3600).
- Changing the lockout interval — same 1-hour cadence whether healthy or in low-battery state. Hourly re-sampling auto-recovers as soon as voltage rises (e.g., solar charging) at negligible cost in sleep mode.
- Per-cycle calibration of the SoC curve. The published curve below is the spec.
- Hard lockout requiring power-cycle or button press to revive. Out of scope.

## Design

### Module: `battery_monitor`

Two new public functions (existing `_init` / `_read_voltage` / `_deinit` unchanged):

```c
/* Convert cell voltage to state-of-charge percentage (0.0–100.0)
 * via piecewise-linear interpolation over the 11-point LiPo curve.
 * Clamps below 3.20V to 0.0 and at/above 4.20V to 100.0.
 * Pure function — host-testable, no ESP-IDF dependencies. */
float battery_monitor_v_to_pct(float volts);

/* Returns true iff volts >= BATTERY_LOW_CUTOFF_V (3.60V).
 * Boundary is inclusive: exactly 3.60V is considered safe. */
bool battery_monitor_is_safe(float volts);
```

Header constant exposed for other modules / tests:
```c
#define BATTERY_LOW_CUTOFF_V  3.60f
```

**SoC lookup table** (11 points, descending):

| Voltage (V) | SoC (%) |
|------------:|--------:|
| 4.20 | 100 |
| 4.05 | 90  |
| 3.96 | 80  |
| 3.90 | 70  |
| 3.85 | 60  |
| 3.80 | 50  |
| 3.76 | 40  |
| 3.73 | 30  |
| 3.70 | 20  |
| 3.65 | 10  |
| 3.20 | 0   |

Stored as a `static const` array inside `battery_monitor.c`. Interpolation walks the table once; under 1 KB total. Exact LUT-point inputs return exact SoC values (no FP slop); intermediate voltages are linearly interpolated between adjacent points.

### Module: `display`

`display_battery_v_to_pct()` becomes a one-line wrapper, eliminating the second SoC implementation:

```c
int display_battery_v_to_pct(float v) {
    return (int)(battery_monitor_v_to_pct(v) + 0.5f);
}
```

New API for the low-battery warning screen:

```c
/* Render a minimal full-screen "LOW BATTERY <volts>V" warning.
 * Intended for one-shot display when battery_monitor_is_safe() is false.
 * Uses large text, no graphics — fast refresh, minimal energy. */
void display_show_low_battery(float volts);
```

Implementation follows the layout style of `display_show_portal()`. Approximate content:

```
+----------------------+
|                      |
|    LOW BATTERY       |
|                      |
|      3.55 V          |
|                      |
|  Charge to resume    |
|                      |
+----------------------+
```

### Module: `main.c`

Two changes.

**1. New early-sample block in `app_main()`**, placed *after* the existing portal-mode check (so GPIO-button and never-provisioned wakes bypass the battery gate naturally — no extra conditionals) and *before* `setup_wifi()`:

```c
/* Zero-load battery sample: must happen before WiFi/MQTT energize.
 * init_system() only touches NVS, event loop, and ADC — no radio yet. */
float ocv = battery_monitor_read_voltage();
ESP_LOGI(TAG, "OCV: %.3fV (%.0f%% SoC)", ocv, battery_monitor_v_to_pct(ocv));

if (!battery_monitor_is_safe(ocv)) {
    ESP_LOGE(TAG, "*** LOW BATTERY %.2fV < %.2fV — skipping WiFi/MQTT ***",
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

RTC-memory latch declared at file scope:
```c
RTC_DATA_ATTR static bool s_low_battery_shown = false;
```

Latching **before** refresh means a brownout reset mid-refresh still leaves the flag set, so subsequent low-battery wakes correctly skip the redraw.

**2. `publish_telemetry_once()` reuses the cached voltage** instead of re-sampling under load:

```c
/* was: float voltage = battery_monitor_read_voltage(); */
float voltage = g_cached_battery_v;
```

`g_cached_battery_v` is a file-static float, set by the early-sample block.

### Battery-check policy: timer wakes only

The low-battery gate applies only to `ESP_SLEEP_WAKEUP_TIMER` (the normal hourly publish path). It is **bypassed** for `ESP_SLEEP_WAKEUP_GPIO` (button press → config portal) and for the never-provisioned first-boot path. Rationale: the user is physically present and intentionally invoking the portal; blocking that on a battery threshold would make the device feel bricked.

Implementation falls out of statement ordering — the early-sample block sits *after* the portal-mode `return`, so portal wakes never reach it. No extra wake-cause conditional required.

### What is deliberately NOT changing

- The ADC oneshot + factory calibration path in `battery_monitor.c`. Already correct.
- The 10-sample averaging loop. Orthogonal to load-sag and useful for ADC noise rejection.
- `init_system()` structure — it already touches only NVS, event loop, and ADC manager, none of which draw meaningful current. No split into early/full phases needed.
- The 3600 s sleep interval.

## Energy / Timing Notes

- E-paper refresh draws ~15 mA for ~2 s (~30 mJ per refresh). The one-shot latch ensures this cost is paid at most once per low-battery episode, not every hour.
- Trade-off: the refresh itself happens when the cell is weakest. If a brownout triggers mid-refresh, the device resets, the RTC latch survives, and the next wake skips the redraw — degraded but recoverable. We accept this; surfacing the warning at all is more valuable than guaranteeing it renders.
- Reading at the top of `app_main()` adds <50 ms (10 ADC samples + cali conversion) to the active window. Negligible vs. the ~5 s WiFi/MQTT time.

## Testing

No on-device test harness exists. Host-side tests (TEST_HOST build, mirroring the existing `display_battery_v_to_pct` pattern):

**`battery_monitor_v_to_pct`:**
- All 11 LUT inputs return their exact tabulated SoC% (no off-by-one from FP rounding).
- Midpoints: e.g. 4.125 V → 95.0 %, 3.825 V → 55.0 %, 3.675 V → 15.0 %.
- Clamp above range: 4.30 V → 100.0, 4.20 V → 100.0.
- Clamp below range: 3.00 V → 0.0, 3.20 V → 0.0.
- NaN / negative input → 0.0 (defensive).

**`battery_monitor_is_safe`:**
- `is_safe(3.59f)` → false
- `is_safe(3.60f)` → true (inclusive boundary)
- `is_safe(3.61f)` → true

**On-device manual verification** (DEVELOPER_GUIDE.md checklist addition):
- Serial log shows `OCV: x.xxxV` line *before* the first WiFi log line.
- Bench supply at 3.50 V at the battery input → low-battery branch fires, e-paper shows "LOW BATTERY 3.50V", device sleeps without WiFi traffic.
- Raise supply to 3.80 V before next wake → normal publish resumes; latch clears; next forced low transition redraws.
- MQTT-published `battery_v` after this change is meaningfully higher (no sag) than before, especially at moderate-to-low SoC.

## Files Touched

| File | Change |
|------|--------|
| `include/battery_monitor.h` | Add `BATTERY_LOW_CUTOFF_V`, `battery_monitor_v_to_pct()`, `battery_monitor_is_safe()` |
| `src/battery_monitor.c` | Implement LUT + interpolation + safety check |
| `include/display.h` | Add `display_show_low_battery()` |
| `src/display.c` | Implement low-battery screen; rewrite `display_battery_v_to_pct()` as wrapper |
| `src/main.c` | Early-sample block, RTC latch, cached voltage in `publish_telemetry_once()` |
| `test/` | New host-side tests for SoC curve and safety check |

## Open Questions

None at design time.
