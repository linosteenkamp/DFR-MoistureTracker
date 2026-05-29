# SP2 — Button + On-Screen Soil Calibration Wizard (Zigbee build)

**Date:** 2026-05-29
**Status:** Design approved
**Depends on:** SP1 (Zigbee transport + managed light-sleep) — merged on `feat/sp1-zigbee-transport`.

## Problem

The WiFi build calibrates the soil sensor through the SoftAP web portal
(`/calibrate`). The Zigbee build has no web portal, so there is currently no way
to capture per-device dry/wet mV thresholds in the field. SP2 adds an on-device
calibration path driven entirely by the single GPIO7 button and the 2.13"
e-paper display.

## Goal

Let a user press GPIO7 to enter an on-screen wizard that captures the dry and wet
raw mV readings and persists them to NVS (`soil_cal` namespace), then returns the
device to normal Zigbee operation — with no phone, web portal, or serial console.

Non-goals (deferred): button-driven Zigbee commissioning/pairing (SP3), any WiFi
teardown (SP3), changes to the WiFi build's existing web-portal calibration.

## Constraints / context

- **Managed light sleep:** the Zigbee build does not deep-sleep. `app_main()`
  joins then returns; `esp_zb` runs the radio with automatic light sleep, so the
  CPU is essentially always-on between radio events.
- **Single button:** GPIO7 only — LP/RTC-capable, momentary push-to-GND with
  internal pull-up.
- **Slow display:** SSD1680 full refresh is ~2 s and cannot smoothly animate a
  live value, so feedback is redraw-on-press, not continuous.
- **Existing storage:** `soil_calibration_save(dry_mv, wet_mv, cal_ts)` →
  NVS `soil_cal`; `cal_ts == 0` means "never calibrated". Capture source is
  `soil_moisture_read_raw_mv()`.
- **No wall clock:** the Zigbee build has no SNTP/RTC time, so `cal_ts` cannot be
  a real timestamp.

## Architecture: reboot into a dedicated calibration mode

Chosen over a concurrent in-stack wizard because calibration is a rare, manual,
local action, so a brief auto-recovering network drop is an acceptable trade for
dramatically simpler, safer code (no concurrency with the live stack, no e-paper
contention with `zb_display_task`, no `esp_zb` sleep juggling).

### Entry

1. In the Zigbee path, GPIO7 is configured with a debounced falling-edge ISR and
   as a **light-sleep wake source** (`gpio_wakeup_enable` +
   `esp_sleep_enable_gpio_wakeup`) so a press wakes the CPU from `esp_zb` managed
   light sleep and fires the ISR.
2. `esp_restart()` is illegal in ISR context, so the ISR gives a semaphore to a
   small trigger task that sets an `RTC_DATA_ATTR` flag (`s_calib_requested = 1`)
   and calls `esp_restart()`. RTC memory survives the soft reset (already relied
   on for `s_low_battery_shown`).
3. On the next boot, `app_main()` checks `s_calib_requested` **before** the
   battery brownout gate — a deliberate press always reaches calibration, which
   is radio-off and low power. It clears the flag immediately (so a crash
   mid-wizard cannot loop), runs the wizard, then `esp_restart()`s back into the
   normal Zigbee path (flag now clear → stack auto-rejoins with persisted creds).

### Wizard flow (state machine)

States: `DRY → WET → SAVE → (reboot)`, plus `ABORT`.

Button events (single button): **SHORT** press (< 2 s), **LONG** press (≥ 2 s),
**TIMEOUT** (30 s idle).

- **DRY** — "Step 1/2: hold sensor in AIR."
  - SHORT: read `soil_moisture_read_raw_mv()`, store pending dry, redraw with the
    captured value (re-press to retry).
  - LONG: accept the captured dry value, advance to WET. (LONG with nothing
    captured yet is ignored.)
  - TIMEOUT: abort.
- **WET** — "Step 2/2: sensor in WATER." Same SHORT/LONG/TIMEOUT semantics;
  LONG advances to SAVE.
- **SAVE** — validate `dry_mv > wet_mv` and both non-zero.
  - Invalid → error screen, return to DRY.
  - Valid → `soil_calibration_save(dry, wet, cal_ts_marker)`, show
    "Saved! dry=X wet=Y" ~3 s, then `esp_restart()`.
- **ABORT** — show "Cancelled" ~2 s, `esp_restart()`, nothing saved.

`cal_ts_marker` is a nonzero constant (e.g. `1`) used purely to flip the existing
`cal_ts != 0` "is calibrated" indicator; it is not a real timestamp in this build.

## Components (designed for isolation + host-testability)

- **`soil_calib_sm.{c,h}`** — *pure* state machine + validation, no ESP
  dependencies. Core entry point:
  `calib_action_t calib_sm_next(calib_state_t *state, calib_event_t event)`
  returning an action ∈ {`NONE`, `CAPTURE_DRY`, `CAPTURE_WET`, `SAVE`, `ABORT`}.
  A pure `calib_values_valid(dry_mv, wet_mv)` performs the range/zero check. Unit
  tested on the `native` env (mirrors `test_zigbee_encode`).
- **`soil_calib_wizard.{c,h}`** — hardware glue. `run_calibration_wizard()` owns
  the display, polls the button (CPU fully on in calib mode → simple debounced
  polling with press-duration timing; no ISR/sleep concerns here), reads the
  sensor, drives the pure SM, and executes its actions (capture via
  `soil_moisture_read_raw_mv()`, persist via `soil_calibration_save()`). Compiled
  in the Zigbee build; entry is Zigbee-only.
- **`display.c` additions** — small focused screens matching the existing
  `display_show_*` style: `display_show_calib_step(step_label, instruction,
  captured_mv /* -1 = none */)`, `display_show_calib_saved(dry, wet)`,
  `display_show_calib_cancelled()`, `display_show_calib_error(msg)`.
- **`main.c`** — GPIO7 ISR + light-sleep wake config in the Zigbee path; the
  trigger task; and the `RTC_DATA_ATTR` flag check + `run_calibration_wizard()`
  fork in `app_main()` (~20 lines, all under `#ifdef USE_ZIGBEE`).

## Data flow

```
[normal Zigbee run, light sleep]
   GPIO7 press --> ISR --> sem --> trigger task: s_calib_requested=1; esp_restart()
        |
        v
[boot] init_system() --> app_main sees s_calib_requested
        clear flag --> run_calibration_wizard()
            loop: draw step --> wait_button_event() --> calib_sm_next()
                  CAPTURE_* --> soil_moisture_read_raw_mv()
                  SAVE      --> validate --> soil_calibration_save() --> screen
                  ABORT     --> screen
        esp_restart()
        |
        v
[boot] s_calib_requested==0 --> normal Zigbee path --> rejoin network
```

## Error handling

- **Invalid capture (0 mV):** `soil_moisture_read_raw_mv()` returning 0 signals a
  hardware/wiring fault (same convention as the web portal); treat as a failed
  capture — do not store it, keep the step active.
- **Invalid range at SAVE (`wet ≥ dry` or zero):** show an error screen and
  return to DRY rather than persisting bad calibration.
- **Idle:** 30 s with no press aborts and reboots without saving.
- **Crash mid-wizard:** the RTC flag is cleared on entry, so a reset re-enters the
  normal Zigbee path rather than looping back into calibration.

## Key risk

GPIO wake **during `esp_zb` managed light sleep** is the only real unknown: it
must reliably wake the CPU and fire the ISR. Mitigation: configure GPIO7 as a
light-sleep wake source in addition to the ISR, and verify on hardware early
(before building the rest of the wizard). Everything downstream (polling in
calib mode, RTC flag, restart fork) is well-trodden.

## Testing

- **Host (`native` env, Unity):** `test_soil_calib_sm` — full transition table
  (SHORT/LONG/TIMEOUT in each state) and `calib_values_valid` (reject `wet ≥ dry`,
  reject zero). Add to the `native` env `test_filter`.
- **On-device (manual):**
  1. Press GPIO7 during normal Zigbee operation → device reboots into the wizard.
  2. Capture dry in air, wet in water; confirm shown mV are plausible.
  3. Long-press through to SAVE; confirm "Saved" values match NVS.
  4. Device reboots and rejoins Zigbee (z2m shows it online, reporting resumes).
  5. Idle 30 s at a step → "Cancelled", reboot, NVS unchanged.
  6. Capture wet ≥ dry → error screen, no save.
