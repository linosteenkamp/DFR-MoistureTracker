# Spec: Soil Moisture Calibration via Config Portal

## Context

`SENSOR_DRY_MV` and `SENSOR_WET_MV` are compile-time constants in `src/soil_moisture.c:63-64`. Every sensor varies — capacitive tolerance, GPIO3 supply sag under load, soil chemistry — so an accurate device today requires editing source, recompiling, and re-flashing. There is no way to re-calibrate after deployment without retrieving the device.

This spec defines a per-device runtime calibration stored in NVS and captured through a SoftAP web portal. The portal is reachable on first boot (no credentials → existing provisioning AP, extended with calibration capture) and on demand via a new push button on GPIO7 (LP-capable → can wake from deep sleep).

The existing GPIO20 hold-button "factory reset" is removed: `factory_reset_check()` is only called from the deprecated `telemetry_loop()` and never runs in the production deep-sleep path, so it is already non-functional. Its responsibility moves into the portal behind a confirm prompt.

## Goals

1. Calibration values are captured per device, not baked into firmware.
2. First-use calibration happens during the existing first-boot SoftAP flow — no extra steps for the user beyond what they already do.
3. Re-calibration is possible at any time without reflashing and without retrieving the device, while preserving the deep-sleep battery model.
4. All configuration actions (WiFi, device ID, calibration, factory reset) live behind a single physical button and a single web UI.
5. A device with no stored calibration still publishes telemetry — it falls back to sensible defaults and logs a warning.

## Non-goals

- Time-based or drift-based re-calibration prompts. Re-calibration is on-demand only.
- Storing historical readings or calibration history. The reserved 16 KB `storage` NVS partition stays unused under this spec.
- Non-linear or multi-point calibration curves.
- OTA firmware updates from the portal.
- Authentication on the SoftAP. The portal remains open WiFi, same as today's provisioning.

## User flows

### First boot

1. Device powers on with no WiFi credentials in NVS. SoftAP `FireBeetle_C6_Prov` starts (existing behaviour).
2. User connects, opens `http://192.168.4.1`.
3. The page now collects WiFi SSID, password, and device ID, then walks the user through *Capture Dry* and *Capture Wet* with a live mV readout.
4. Submit writes WiFi + device ID + calibration to NVS, then restarts. Normal hourly telemetry resumes.

### Re-configuration after deployment

1. User presses the GPIO7 button. Device wakes from deep sleep via LP-GPIO wake. Wake cause is `ESP_SLEEP_WAKEUP_GPIO`.
2. Main loop branches into **portal mode**: telemetry is skipped for this cycle. SoftAP comes up.
3. User connects to the portal and chooses from: Calibrate, Status, Change WiFi, Factory reset.
4. On save or after a 10-minute idle timeout, device returns to deep sleep. The next timer wake follows the normal telemetry path.

## Requirements

### Functional

- **R1.** Calibration values (`dry_mv`, `wet_mv`) are persisted in NVS and survive deep sleep, power cycle, and firmware reflash (provided NVS is not erased).
- **R2.** A `soil_moisture_read_raw_mv()` API exposes the averaged ADC reading in millivolts without applying the percentage conversion, so the portal can show and capture raw values.
- **R3.** The portal serves four logical sections: WiFi/device-ID config, calibration capture, status (read-only), factory reset. Each has its own URL.
- **R4.** Calibration capture takes multi-sample averaged readings (re-using the existing 10-sample average in `soil_moisture_read_voltage`).
- **R5.** A GPIO7 press during deep sleep wakes the device directly into portal mode, bypassing the telemetry path for that cycle.
- **R6.** First-boot provisioning includes calibration capture in the same submission as WiFi credentials.
- **R7.** Factory reset (from the portal) wipes both the WiFi and calibration NVS namespaces, then restarts.
- **R8.** If calibration is absent from NVS at boot, the device proceeds with default values (`2800` mV dry, `0` mV wet — matching today's constants) and logs a warning. Telemetry continues to publish.

### Non-functional

- **N1.** Deep-sleep current draw remains under ~20 µA. The GPIO7 wake configuration must not hold a pull-up active during sleep beyond what the wake source itself requires.
- **N2.** No new external dependencies. HTML is inline; JSON is hand-rolled with `snprintf`, matching the existing pattern in `mqtt_publisher.c`.
- **N3.** The portal session has the same 10-minute idle timeout as today's `PROVISIONING_TIMEOUT_SEC` to bound SoftAP power draw.
- **N4.** Existing telemetry payload format is unchanged — no new fields are added to MQTT under this spec.
- **N5.** Pure-logic code (form parsing, percentage math, calibration fallback) is covered by host-native unit tests runnable via `pio test -e native`. NVS round-trip behaviour for `soil_calibration` is covered by on-device tests runnable via `pio test -e dfrobot_firebeetle2_esp32c6`.

## Architecture

Two new modules replace two retired ones:

| Module | Status | Responsibility |
|---|---|---|
| `soil_calibration` | New | Owns the in-RAM calibration values. Reads from / writes to the `soil_cal` NVS namespace. Provides getters consumed by `soil_moisture`. |
| `config_portal` | New (replaces `wifi_provisioning`) | SoftAP lifecycle, HTTP server, all request handlers (WiFi form, calibration capture, status, factory reset). |
| `soil_moisture` | Modified | Drops the compile-time `SENSOR_DRY_MV` / `SENSOR_WET_MV` constants and pulls values at runtime from `soil_calibration`. Exposes `soil_moisture_read_raw_mv()`. |
| `main` | Modified | Branches on wake cause; configures GPIO7 deep-sleep wake; initialises `soil_calibration` after NVS. |
| `factory_reset` | Retired | GPIO20 button removed; functionality moves into the portal. |
| `wifi_provisioning` | Retired | Folded into `config_portal`. |

### Dependencies and ordering

`soil_calibration_init()` must run after NVS is initialised and before `soil_moisture_init()` (since `soil_moisture` reads calibration at runtime). The init order becomes: NVS → ADC manager → `soil_calibration` → battery monitor → soil moisture.

### Reuse

- WiFi credential helpers in `wifi_credentials.c` (`save`, `load`, `save_device_id`, `load_device_id`, `clear`) are reused by the new portal handlers unchanged.
- The form parser in `wifi_provisioning.c:57-88` (`copy_form_field`, `parse_form_fields`, `url_decode`) moves into `config_portal.c`, extended for additional fields.
- The SoftAP setup in `wifi_provisioning.c:184-233` moves into `config_portal_run()` verbatim.
- `soil_moisture_read_voltage()`'s 10-sample averaging path is the basis for `soil_moisture_read_raw_mv()` — the new function returns the integer mV from the same code path.
- `mqtt_publisher.c`'s `snprintf` JSON pattern is the model for the `/api/reading` response.

## NVS schema

New namespace `soil_cal` in the default `nvs` partition (the reserved `storage` partition stays untouched).

```
soil_cal/
  dry_mv  : u32   // ADC mV in open air, captured by user
  wet_mv  : u32   // ADC mV submerged in water, captured by user
  cal_ts  : u32   // boot-counter or epoch-ish timestamp, status-page display only
```

Defaults on missing keys: `dry_mv = 2800`, `wet_mv = 0`, `cal_ts = 0`. A warning is logged and telemetry continues.

The existing `wifi_config` namespace is unchanged.

## HTTP API

All HTML pages use the same inline-style approach as today's `wifi_provisioning.c`. Form bodies are URL-encoded; JSON is hand-rolled.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | Main menu page; links to each section. |
| `GET` | `/wifi` | WiFi + device ID form. |
| `POST` | `/wifi` | Save WiFi + device ID to NVS. |
| `GET` | `/calibrate` | Calibration page. Client polls `/api/reading` for live mV. |
| `GET` | `/api/reading` | JSON `{raw_mv, percentage, dry_mv, wet_mv}` — current sensor read against stored cal. |
| `POST` | `/api/calibrate/dry` | Multi-sample read; returns captured mV; stashes pending value in RAM. |
| `POST` | `/api/calibrate/wet` | Same, wet point. |
| `POST` | `/api/calibrate/save` | Commit pending dry+wet to NVS; update `cal_ts`. |
| `GET` | `/status` | Read-only HTML: stored `dry_mv`, `wet_mv`, last raw mV, last percentage, `cal_ts`. |
| `POST` | `/factory-reset` | Wipe `wifi_config` and `soil_cal` namespaces; restart. Confirm prompt rendered client-side. |

## Wake / boot logic

Pseudocode showing the new branch in `main.c`:

```c
app_main() {
    init_system();   // NVS, ADC, soil_calibration_init, sensors
    wake_cause = esp_sleep_get_wakeup_cause();

    if (wake_cause == ESP_SLEEP_WAKEUP_GPIO || !wifi_credentials_is_provisioned()) {
        config_portal_run();          // blocks until save or 10-min timeout
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;
    }

    // existing telemetry path, unchanged
    setup_wifi();
    setup_mqtt();
    publish_telemetry_once();
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
}

enter_deep_sleep(s) {
    // existing teardown unchanged
    esp_deep_sleep_enable_gpio_wakeup(BIT(GPIO_NUM_7), ESP_GPIO_WAKEUP_GPIO_LOW);
    gpio_hold_en(GPIO_NUM_3);
    esp_deep_sleep_start();
}
```

## Hardware

- **GPIO7** — LP-capable, currently unused. Momentary push-to-GND button. Internal pull-up enabled at wake-configuration time; wake on LOW. A 0.1 µF debounce capacitor is optional and deferred until spurious wakes are observed in practice.
- **GPIO20** — existing factory-reset button retired. The pin becomes unused; the existing `gpio_set_pull_mode(GPIO_NUM_20, GPIO_FLOATING)` call in `enter_deep_sleep()` stays so the pad doesn't leak through its internal pull-up.

## Verification

This change introduces automated test coverage for the new modules. The existing manual checklist remains for hardware- and network-dependent behaviour that automation cannot reach.

### Automated tests

PlatformIO Unit Testing is already scaffolded in the `test/` directory but unused. This spec activates it.

**Host-native (`pio test -e native`)** — adds an `[env:native]` to `platformio.ini` and Unity-based test files under `test/native/`:

- `test_form_parser` — exercises `parse_form_fields`, `copy_form_field`, and `url_decode` lifted into `config_portal.c`. Covers happy path, missing fields, oversized fields, URL-encoded values.
- `test_percentage_math` — exercises `soil_moisture` percentage conversion at the dry boundary, wet boundary, midpoint, and out-of-range inputs. The math is factored into a pure function that takes (raw_mv, dry_mv, wet_mv) so it is callable without hardware.
- `test_calibration_fallback` — exercises `soil_calibration` default-fallback behaviour with NVS abstracted behind a thin shim header (`nvs_shim.h`) that has a host stub implementation. Confirms `dry_mv = 2800` / `wet_mv = 0` returned when keys are absent, and that user-set values override the defaults.

**On-device (`pio test -e dfrobot_firebeetle2_esp32c6`)** — Unity test files under `test/embedded/`:

- `test_calibration_nvs` — round-trips real `dry_mv` / `wet_mv` values through `soil_calibration_save()` / `soil_calibration_init()` against the actual NVS partition. Test fixture erases the `soil_cal` namespace before and after each case to keep runs reproducible.

### Manual verification

End-to-end procedure with serial monitor, for behaviour outside the automated harness:

1. **First-boot flow.** Erase flash, flash firmware, connect to SoftAP, complete WiFi + calibration form (capture dry in air, capture wet at the MAX line). Confirm device restarts and publishes telemetry with a sane moisture percentage in actual soil (30–60% typical).
2. **GPIO7 wake.** After ≥1 deep-sleep cycle, press the GPIO7 button. Verify serial log reports wake cause `ESP_SLEEP_WAKEUP_GPIO`, portal mode starts, telemetry is skipped this cycle.
3. **Re-calibrate only.** From the portal, open `/calibrate`. Confirm `/api/reading` polling shows live values that respond to wetting/drying. Capture new dry + wet, save. Next timer wake should publish telemetry under the new curve.
4. **Status page.** `/status` shows the values that were just captured.
5. **Change WiFi without losing calibration.** From the portal, change SSID and password. Save → restart → device reconnects on the new network. Status page shows calibration values are unchanged.
6. **Factory reset.** Trigger from the portal. Device restarts into first-boot flow; status page (after re-provisioning) shows default calibration.
7. **Sleep current.** Multimeter on the device in deep sleep: confirm <20 µA. Press GPIO7; confirm wake within a few seconds.
8. **Portal timeout.** Press GPIO7, leave the portal idle for 10 minutes → device returns to deep sleep cleanly; next timer wake publishes telemetry normally.

## Open questions

None at the time of writing. Decisions resolved during brainstorming:

- Trigger UX: first-boot SoftAP + GPIO7 wake-to-portal (not MQTT command, not separate calibration mode).
- Re-cal cadence: on-demand only (no time-based nags, no drift detection).
- Web UI scope: calibrate, view status, change WiFi, factory reset — all four.
