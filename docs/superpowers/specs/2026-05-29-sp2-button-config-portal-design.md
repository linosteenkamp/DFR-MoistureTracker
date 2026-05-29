# SP2 — Button-Triggered Config Portal: Calibration + Sensor Name (Zigbee build)

**Date:** 2026-05-29
**Status:** Design approved (revised from the earlier e-paper-wizard draft)
**Depends on:** SP1 (Zigbee transport + managed light-sleep) — on `feat/sp1-zigbee-transport`.

## Problem

The WiFi build calibrates the soil sensor through the SoftAP web portal
(`/calibrate`). The Zigbee build disables that portal (`#ifndef USE_ZIGBEE`), so
in the field there is currently no way to (a) capture per-device dry/wet mV
thresholds, or (b) give a sensor a human-readable name. Names matter because the
downstream automation runs in **Node-RED off the MQTT messages** z2m publishes —
Node-RED needs a stable, device-authoritative identifier in the payload to route
logging and actions to the right physical sensor.

## Goal

A GPIO7 press reboots the Zigbee device into a SoftAP **config portal** (no WiFi
station setup) that lets the user, from a phone, (1) calibrate the soil sensor
and (2) set a sensor name. The name is persisted and surfaced both on the
e-paper display and **in the z2m MQTT payload** so Node-RED can identify the
sensor. The device then reboots back into normal Zigbee operation.

Non-goals (deferred to SP3): button-driven Zigbee commissioning/pairing, WiFi
station teardown / config cleanup. The WiFi build's existing portal behavior is
unchanged.

### Why a web portal instead of an on-screen button wizard

The original SP2 draft proposed a single-button e-paper wizard. It was dropped
because: (a) a friendly name needs **text entry**, impossible with one button and
a ~2 s e-paper refresh; (b) the web portal's calibration UX is already proven and
gives a **live mV readout** (JS polling `/api/calibrate`) that the e-paper cannot;
(c) it reuses `config_portal.c` / `form_parser` instead of a new state machine.
The button's role shrinks to a trigger.

## Architecture: reboot into a dedicated config-portal mode

Reuses the SP1-chosen reboot-into-mode pattern. Because Zigbee is **not
initialized** in config mode, only the WiFi SoftAP radio is active — there is no
WiFi/802.15.4 coexistence problem.

### Entry / exit

1. In the Zigbee path, GPIO7 is configured with a debounced falling-edge ISR and
   as a **light-sleep wake source** (`gpio_wakeup_enable` +
   `esp_sleep_enable_gpio_wakeup`) so a press wakes the CPU from `esp_zb` managed
   light sleep and fires the ISR.
2. `esp_restart()` is illegal in ISR context, so the ISR gives a semaphore to a
   small trigger task that sets an `RTC_DATA_ATTR` flag (`s_config_requested = 1`)
   and `esp_restart()`s. RTC memory survives the soft reset (already relied on for
   `s_low_battery_shown`).
3. On the next boot, `app_main()` checks `s_config_requested` **before** the
   battery brownout gate (a deliberate press should always reach config; the radio
   is off so power is modest). It clears the flag immediately (a crash mid-portal
   then returns to normal rather than looping), then runs the portal:
   `display_show_portal()` + `config_portal_run()` (blocks until save or timeout).
4. On portal exit, `esp_restart()` back into the normal Zigbee path (flag now
   clear → stack auto-rejoins with persisted credentials).

This mirrors the WiFi build's existing `run_portal_then_sleep()`, except it
`esp_restart()`s instead of deep-sleeping, and runs the Zigbee variant of the
portal pages.

## Config portal (Zigbee variant)

`config_portal.c` gains a `USE_ZIGBEE` variant of its landing page:

- **Hidden:** the WiFi SSID/password fields (the Zigbee build has no WiFi STA).
- **Sensor name:** reuse the existing `device_id` NVS field
  (`wifi_credentials_save_device_id` / `_load_device_id`), relabeled "Sensor
  name" in the UI. **Max 16 characters** (ZCL LocationDescription limit, see
  below). This avoids a new NVS key; in the Zigbee build `device_id` is otherwise
  used only as the e-paper label.
- **Calibrate Sensor:** unchanged — reuses the existing `/calibrate`,
  `/api/calibrate/{dry,wet,save}` handlers and live mV readout.
- **Status / factory reset:** unchanged.

The shared calibration and form-parsing handlers are not modified; only the
landing page markup and the device-id field's label/validation differ under
`#ifdef USE_ZIGBEE`.

## Sensor name in the MQTT payload

The name must be device-authoritative and land in the z2m JSON so Node-RED can
route on it. Path:

1. **Device side:** add the Basic cluster **LocationDescription** attribute
   (0x0010, standard writable character string, **max 16 octets**) to the
   endpoint in `zigbee_reporter.c`. Custom clusters assert in this SDK (SP1), so a
   standard Basic attribute is the only safe vehicle. At stack startup, load
   `device_id` from NVS and set LocationDescription to it.
2. **z2m converter:** extend `z2m/dfr_soil_moisture.js` with a small `fromZigbee`
   that maps `genBasic.locationDesc` → a `label` field, an `exposes` text entry,
   and a `configure` that reads `locationDesc` once (and requests reporting if the
   SDK supports reporting on the string attribute). z2m caches read attributes in
   device state and republishes them, so `label` rides along in subsequent MQTT
   messages.

Result: every `zigbee2mqtt/<friendly_name>` payload includes
`"label": "<sensor name>"`, independent of the z2m-side friendly_name. Node-RED
switches on `msg.payload.label`.

> **Simpler alternative considered (rejected):** rely solely on z2m's own
> friendly_name in the topic, set once per device in the z2m UI. Rejected because
> the user wants the identifier to be authoritative on the device and present in
> the payload, not configured separately in z2m.

## Components / changes

- **`main.c`** (`#ifdef USE_ZIGBEE`): GPIO7 ISR + light-sleep wake config in the
  Zigbee path; the trigger task; the `RTC_DATA_ATTR` flag check + portal fork in
  `app_main()` (run portal, then `esp_restart()`).
- **`config_portal.c`**: `USE_ZIGBEE` landing-page variant (hide WiFi fields,
  relabel device-id as "Sensor name", 16-char cap). No change to calibration
  handlers.
- **`zigbee_reporter.c`**: add Basic LocationDescription (0x0010) attribute; set
  it from NVS `device_id` at startup; expose it for read (+ reporting if
  supported).
- **`z2m/dfr_soil_moisture.js`**: expose `label` from `genBasic.locationDesc`.
- **No new C modules**, and the e-paper wizard / pure state machine from the prior
  draft are dropped.

## Data flow

```
[normal Zigbee run, light sleep]
   GPIO7 press --> ISR --> sem --> trigger task: s_config_requested=1; esp_restart()
        |
        v
[boot] init_system() --> app_main sees s_config_requested
        clear flag --> display_show_portal() + config_portal_run()
            user: calibrate (dry/wet -> soil_calibration_save)
                  set sensor name (-> wifi_credentials_save_device_id)
        esp_restart()
        |
        v
[boot] s_config_requested==0 --> normal Zigbee path
        zigbee_reporter sets Basic.LocationDescription = device_id
        rejoin --> periodic reports
        |
        v
[z2m] reads locationDesc --> payload includes {"label": "<name>"} --> Node-RED
```

## Error handling

- **Invalid capture (0 mV):** existing portal convention — surfaced as an error,
  not saved.
- **Portal timeout / no save:** `config_portal_run()` returns; device reboots into
  normal Zigbee operation with prior NVS values intact.
- **Crash mid-portal:** RTC flag is cleared on entry, so a reset returns to normal
  Zigbee rather than looping into config mode.
- **Name length:** UI caps at 16 chars; device truncates/validates before writing
  LocationDescription.

## Key risks

1. **GPIO wake during `esp_zb` managed light sleep** — the press must reliably
   wake the CPU and fire the ISR. Mitigation: configure GPIO7 as a light-sleep
   wake source in addition to the ISR; verify on hardware first.
2. **WiFi SoftAP in the Zigbee firmware** — config mode runs `esp_wifi` SoftAP
   with Zigbee uninitialized. Verify the Zigbee sdkconfig links WiFi and SoftAP
   comes up; flash headroom is ample (SP1 image ~18% of 4 MB). No coexistence
   issue since the radios are not active simultaneously.
3. **z2m surfacing LocationDescription** — needs a custom converter snippet and
   relies on z2m reading the string attribute. Validate that `label` appears in
   the published payload; fall back to read-on-demand if string reporting is
   unsupported.

## Testing

- **Host (`native` env, Unity):** no new pure module in this revision; existing
  tests remain. (Form parsing already covered by `test_form_parser`.)
- **On-device (manual):**
  1. Press GPIO7 during normal Zigbee operation → device reboots into the SoftAP
     portal (`FireBeetle_C6_Prov`), no WiFi-credential fields shown.
  2. Calibrate dry-in-air / wet-in-water; confirm saved values via status page.
  3. Set a sensor name; save.
  4. Device reboots and rejoins Zigbee; e-paper shows the new name.
  5. In z2m, confirm the device payload includes `"label": "<name>"`.
  6. Portal timeout with no changes → reboot, NVS unchanged.
