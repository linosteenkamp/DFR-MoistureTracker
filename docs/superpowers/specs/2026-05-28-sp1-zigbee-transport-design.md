# SP1 — Zigbee Transport Bring-up

**Date:** 2026-05-28
**Status:** Approved (pre-implementation)
**Part of:** Zigbee migration initiative (SP1 of 3 — see "Relationship to SP2/SP3" below)
**Scope:** New `zigbee_reporter` module, `main.c` transport fork, `partitions.csv`, build config, a zigbee2mqtt external converter. Reuses `battery_monitor`, `soil_moisture`, `soil_calibration`, `display` unchanged.

## Problem

The soil sensors are the only WiFi devices in an otherwise all-Zigbee, zigbee2mqtt-based home automation setup. WiFi is used purely as an MQTT transport — the device even publishes to a `zigbee2mqtt/{device_id}` topic, imitating a Zigbee device it isn't. The ESP32-C6 already has an 802.15.4 radio and Zigbee 3.0 support on the same die; the WiFi radio is used only because the firmware was written that way.

SP1 makes the device a *native* Zigbee end-device: it joins the Zigbee network and reports soil moisture and battery via standard ZCL clusters, appearing in zigbee2mqtt alongside the real Zigbee devices. This eliminates the "is the WiFi router up?" failure mode, removes the WiFi-portal provisioning friction (addressed fully in SP3), and lets the device mesh through the existing Zigbee network.

## Goals

- Join the Zigbee network as a sleepy end-device and report soil moisture + battery via standard ZCL clusters.
- Report every 15 minutes; deep-sleep between reports; stay within a 32-minute end-device timeout so the parent never ages the device out (no routine rejoins).
- Standard clusters only — Soil Moisture Measurement (0x0408) and Power Configuration (0x0001) — implemented correctly so a minimal zigbee2mqtt converter suffices.
- Keep WiFi as the default build. Zigbee lives behind a build flag (`USE_ZIGBEE`) so the device stays shippable throughout SP1.
- Reuse the existing sensor, calibration, battery, and display modules with no behavioral change.

## Non-Goals (deferred to SP2/SP3)

- Button-driven commissioning UX and the calibration wizard — **SP2/SP3**. SP1 uses crude auto-commissioning (BDB steering on boot when not joined).
- Removal of WiFi/MQTT/config-portal/wifi_credentials code — **SP3**. SP1 leaves it intact behind the build flag.
- On-device calibration over Zigbee (writable ZCL attributes) — out of scope. Calibration in SP1 still uses whatever the WiFi build provides; the Zigbee build just reads the stored calibration.
- Solving battery-SoC inflation during solar charging (see "Known limitations").
- WiFi/802.15.4 runtime coexistence. The two transports are mutually exclusive at build time.

## Background: hardware power context

Each sensor has a 5 V 500 mA solar panel charging the LiPo. Average device draw is a few mA against up to ~2.5 W of daylight charging, so **power is not the binding design constraint**. The reporting cadence is chosen for data freshness and Zigbee network behavior, not battery life. Soil moisture changes slowly (hours–days), so 15-minute reporting is already more than sufficient for the data.

## Design

### Sleep / power model

Deep sleep between reports, with the report interval deliberately nested inside the Zigbee end-device timeout:

- **Report interval: 15 min.** **End-device timeout: 32 min** (a conservative, widely-honored enum value).
- Because 15 min < 32 min with 2× margin, the parent keeps the device in its child table across each sleep. The device restores persisted network state on wake and reports **without a full rejoin**. A rejoin only happens if a report is missed (e.g. RF interference) long enough to exceed the timeout — the rare fallback path.
- Deep sleep (full power-down, ~10–20 µA) is used rather than Zigbee light-sleep, since light-sleeping for 15 min would draw far more than deep sleep + a cheap wake.

This is the "persisted network state" model (vs rejoin-every-wake), made robust by keeping the interval inside the timeout. The ESP Zigbee SDK's deep-sleep support with NVRAM retention is the mechanism; verifying it behaves as described on the C6 is the top SP1 risk (see Risks).

### ZCL data model

Device is a Zigbee 3.0 End Device with one application endpoint hosting:

| Cluster | ID | Role | Attributes used |
|---|---|---|---|
| Basic | 0x0000 | server | ManufacturerName, ModelIdentifier, PowerSource (=battery), ZCLVersion |
| Identify | 0x0003 | server | IdentifyTime (mandatory for commissioning/UX) |
| Power Configuration | 0x0001 | server | BatteryVoltage (0x0020), BatteryPercentageRemaining (0x0021) |
| Soil Moisture Measurement | 0x0408 | server | MeasuredValue (0x0000), MinMeasuredValue (0x0001), MaxMeasuredValue (0x0002) |

**Attribute encodings (these are the host-testable pure functions):**

- **Soil moisture** `MeasuredValue` — uint16, value = `round(pct × 100)`, range 0–10000 (i.e. 0.00–100.00 %). MinMeasuredValue = 0, MaxMeasuredValue = 10000.
- **Battery voltage** `BatteryVoltage` — uint8, units of 100 mV, value = `round(volts × 10)` (0.1 V resolution; e.g. 3.85 V → 39). Coarse but standard.
- **Battery percentage** `BatteryPercentageRemaining` — uint8, units of 0.5 %, value = `round(pct × 2)`, range 0–200 (e.g. 84 % → 168).

SoC % comes from the existing `battery_monitor_v_to_pct()`. The voltage comes from the existing zero-load OCV sample.

### Commissioning (SP1 scope: automatic)

On boot, if the device is not already joined (no persisted network), it runs BDB network steering and joins whatever network has `permit_join` open. No button interaction. The operator enables `permit_join` in zigbee2mqtt, powers the device, and it joins. This is intentionally crude — the button-driven choose-screen commissioning is SP3.

### Boot/wake flow (USE_ZIGBEE build)

```
app_main():
  1. init_system()            // NVS, event loop, ADC manager, sensors (WiFi netif NOT started)
  2. early OCV sample + brownout gate   // unchanged from current firmware; skip-and-sleep if < 3.70 V
  3. zigbee_reporter_init()   // start ESP Zigbee stack; restore network or BDB-steer if unjoined
  4. wait for network-ready (bounded timeout)
  5. read soil moisture + reuse cached OCV
  6. zigbee_reporter_report(soil_pct, battery_v, battery_pct)
        - encode + set the three attribute values
        - trigger attribute report to the coordinator
        - brief drain wait for TX completion
  7. display_show_telemetry(...)   // unchanged
  8. enter_deep_sleep_zigbee(15 min)   // persist Zigbee NVRAM, then timer-wake deep sleep
```

The brownout gate (skip reporting when OCV < 3.70 V) carries over. With solar this rarely trips, but it stays as a safety net.

### Module structure

| File | Action | Responsibility |
|---|---|---|
| `src/zigbee_reporter.c` + `include/zigbee_reporter.h` | **Create** | ESP Zigbee stack init, endpoint/cluster registration, network join/restore, attribute encode+report, Zigbee-aware deep sleep |
| `src/zigbee_encode.c` + `include/zigbee_encode.h` | **Create** | Pure encoders: `zigbee_encode_soil_pct(float)→uint16`, `zigbee_encode_batt_voltage(float)→uint8`, `zigbee_encode_batt_pct(float)→uint8`. Host-testable, no SDK deps. |
| `src/main.c` | Modify | `#ifdef USE_ZIGBEE` transport fork in `app_main()`; Zigbee-aware deep sleep path |
| `partitions.csv` | Modify | Add `zb_storage` (Zigbee NVRAM) and `zb_fct` (Zigbee factory) data partitions required by the SDK |
| `platformio.ini` | Modify | New build env or build flag exposing `-DUSE_ZIGBEE`; declare ESP Zigbee component dependency |
| `main/idf_component.yml` (or PIO equivalent) | **Create** | Declare `espressif/esp-zigbee-lib` + `espressif/esp-zboss-lib` managed-component deps |
| `src/CMakeLists.txt` | Modify | Register the two new `.c` files |
| `test/test_zigbee_encode/test_zigbee_encode.c` | **Create** | Unity host tests for the pure encoders |
| `z2m/dfr_soil_moisture.js` | **Create** | zigbee2mqtt external converter (modernExtend: `soilMoisture()` + `battery()`) |

`mqtt_publisher.c`, `wifi_*`, and `config_portal.c` are untouched in SP1 — they remain the active path in the default (non-`USE_ZIGBEE`) build.

### zigbee2mqtt external converter

Because the device implements standard clusters, the converter is minimal — roughly:

```javascript
const {soilMoisture, battery, identify} = require('zigbee-herdsman-converters/lib/modernExtend');

module.exports = [{
    zigbeeModel: ['DFR-SoilSensor'],          // matches Basic cluster ModelIdentifier
    model: 'DFR-SoilSensor',
    vendor: 'DFRobot-DIY',
    description: 'ESP32-C6 soil moisture + battery sensor',
    extend: [
        soilMoisture(),                        // cluster 0x0408 -> soil_moisture %
        battery(),                             // cluster 0x0001 -> battery %, voltage
        identify(),
    ],
}];
```

The exact modernExtend signatures are verified against the installed zigbee-herdsman-converters version during implementation (the API evolves). ModelIdentifier in the Basic cluster must match `zigbeeModel`.

## Known limitations (documented, not solved in SP1)

- **Battery SoC inflation during solar charging.** When the panel charges, the LiPo terminal sits at the charger CV voltage (up to 4.2 V) regardless of true SoC, so the reported percentage reads high in daylight. The existing zero-load OCV sampling addresses WiFi-load *sag*, not charge-current *elevation*. SP1 reports voltage + SoC as-is; a future refinement could detect charging or trust only pre-dawn readings.
- **Crude commissioning.** Auto-steer-on-boot only; proper button UX is SP3.

## Risks (verify early in implementation)

1. **ESP Zigbee SDK deep-sleep + NVRAM retention on C6.** The whole power model depends on the device restoring network state across deep sleep and reporting without a rejoin. This is the highest-risk item — validate with a minimal sleep/report loop before building the full data model.
2. **Coordinator honoring a 32-min end-device timeout.** If the adapter ages the device out sooner, we fall back to rejoin-every-wake (still works, just more churn). Measure actual behavior against the real coordinator early.
3. **PlatformIO + ESP-IDF managed components.** `esp-zigbee-lib`/`esp-zboss-lib` come via the ESP Component Registry. PlatformIO's component-manager integration can be finicky; confirm a clean build before writing application code.
4. **zigbee2mqtt converter correctness for 0x0408.** modernExtend `soilMoisture()` API may differ across z2m versions; verify against the installed version.

## Testing

### Host tests (TEST_HOST / `pio test -e native`)

`test_zigbee_encode` — pure encoder coverage:
- `zigbee_encode_soil_pct`: 0 %→0, 100 %→10000, 45.5 %→4550, clamp >100 %→10000, clamp <0→0.
- `zigbee_encode_batt_voltage`: 3.85 V→39 (rounding), 4.20 V→42, 3.20 V→32.
- `zigbee_encode_batt_pct`: 0 %→0, 100 %→200, 84 %→168, clamp >100 %→200.

The Zigbee stack itself is hardware/SDK-bound and not host-testable; only the encoders are unit-tested.

### On-device verification (manual, no HIL harness)

- Device performs BDB steering and appears in zigbee2mqtt as `DFR-SoilSensor` after `permit_join`.
- `soil_moisture` and `battery` values appear and track reality (probe in air vs water; battery vs multimeter).
- Device survives ≥3 consecutive sleep→wake→report cycles without orphaning (no rejoin logged in z2m).
- Reported `soil_moisture` matches the device's own `display_show_telemetry` value.
- Battery-life trend observed over a few days (sanity, with solar).

## Relationship to SP2/SP3

- **SP2 — Button input + calibration wizard.** Independent of transport. Builds `button.c` (press-duration state machine) + `cal_wizard.c` + display screens. Carries forward the wizard design already sketched (two-point dry/wet capture, validation `dry_mv > wet_mv`, per-screen timeout, LED long-press feedback).
- **SP3 — Commissioning UX + WiFi teardown.** Depends on SP1 + SP2. Adds the button "choose" screen (short→calibrate, long→Zigbee pair), wires the pair branch to the SP1 commissioning code, deletes WiFi/MQTT/config-portal/wifi_credentials, flips `USE_ZIGBEE` to the default, and cleans up NVS.

## Open Questions

None at design time.
