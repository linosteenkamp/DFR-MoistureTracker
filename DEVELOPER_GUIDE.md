# Developer Documentation - DFR-MoistureTracker

## Overview

This document provides technical details for developers maintaining or extending the DFR-MoistureTracker codebase.

## Project Structure

```
DFR-MoistureTracker/
├── include/              # Public header files
│   ├── adc_manager.h     # Shared ADC resource management
│   ├── battery_monitor.h # Battery voltage reading interface
│   ├── soil_moisture.h   # Soil moisture sensor interface
│   ├── wifi_credentials.h # NVS credential storage interface
│   ├── wifi_manager.h    # WiFi connection management
│   ├── wifi_provisioning.h # Provisioning mode interface
│   ├── mqtt_publisher.h  # MQTT client and publishing
│   └── factory_reset.h   # Factory reset button interface
├── src/                  # Implementation files
│   ├── main.c            # Application entry point
│   ├── adc_manager.c     # ADC resource sharing implementation
│   ├── battery_monitor.c # Battery ADC reading implementation
│   ├── soil_moisture.c   # Soil sensor reading implementation
│   ├── wifi_credentials.c # NVS read/write implementation
│   ├── wifi_manager.c    # WiFi STA mode implementation
│   ├── wifi_provisioning.c # SoftAP + HTTP server implementation
│   ├── mqtt_publisher.c  # MQTT client implementation
│   ├── factory_reset.c   # GPIO button monitoring implementation
│   └── CMakeLists.txt    # Build configuration
├── partitions.csv        # Flash memory layout
├── platformio.ini        # PlatformIO project configuration
├── CMakeLists.txt        # Top-level ESP-IDF build config
└── *.md                  # Documentation files
```

## Architecture

### Design Principles

The codebase follows **SOLID principles**:

1. **Single Responsibility**: Each module has one clear purpose
2. **Open/Closed**: Easy to extend without modifying existing code
3. **Liskov Substitution**: Interfaces are consistent and predictable
4. **Interface Segregation**: Minimal, focused interfaces
5. **Dependency Inversion**: Depends on abstractions, not implementations

### Module Dependency Graph

```
                           main.c
                              |
        ┌─────────────────────┼─────────────────────┐
        |                     |                     |
    wifi_manager      adc_manager           mqtt_publisher
        |                     |                     |
   wifi_credentials    ┌──────┴──────┐       (mqtt library)
        |              |             |
    (nvs_flash)  battery_monitor  soil_moisture
                       |             |
                   (esp_adc)    (esp_adc)
```

### Key Subsystems

#### 1. ADC Resource Manager (`adc_manager.c`)
**Purpose**: Centralized ADC unit management

**Why it exists**: ESP32-C6 ADC units cannot be initialized multiple times. Battery monitor and soil moisture sensor both need ADC1, so this module provides a shared handle.

**Key Functions**:
- `adc_manager_init()` - Create ADC unit (call once at startup)
- `adc_manager_get_handle()` - Get shared ADC handle
- `adc_manager_create_cali()` - Get/create calibration handle

**Extension Point**: 
- Add more channels: Simply configure additional channels using the shared handle
- Support ADC2: Create a second unit handle (requires significant changes)

#### 2. Sensor Modules (`battery_monitor.c`, `soil_moisture.c`)
**Pattern**: Both follow the same structure

**Initialization**:
1. Get ADC handle from manager
2. Configure channel (GPIO, attenuation)
3. Get/create calibration handle
4. Set initialized flag

**Reading**:
1. Check initialized flag
2. Take multiple samples
3. Average for noise reduction
4. Apply calibration
5. Return value

**Extension Point**:
- Add new ADC sensor: Copy structure from battery_monitor.c
- Change sample count: Modify `SAMPLE_COUNT` define
- Implement filtering: Add moving average or Kalman filter in read function

#### 3. WiFi Subsystem
**Components**:
- `wifi_credentials.c` - NVS persistence
- `wifi_manager.c` - STA mode connection
- `wifi_provisioning.c` - AP mode + HTTP server

**State Machine**:
```
Boot → Check NVS → [Provisioned?]
                         |
              No ←───────┴───────→ Yes
              |                    |
       Start Provisioning    Connect WiFi
              |                    |
       Wait for Config      [Connected?]
              |                    |
         Save to NVS          Yes → Continue
              |                    |
           Restart               No → Clear & Restart
```

**Extension Point**:
- Add BLE provisioning: Implement alongside HTTP provisioning
- Add custom settings: Extend wifi_credentials to store additional data
- Change AP name: Modify `PROV_AP_SSID` in wifi_provisioning.c

#### 4. MQTT Publisher
**Responsibilities**:
- Connect to broker
- Maintain connection
- Publish telemetry JSON

**JSON Structure**:
```json
{
  "battery": 4.15,
  "soil_moisture": 67.5,
  "device": "sensor02"
}
```

**Extension Point**:
- Add more fields: Modify `mqtt_publisher_publish_telemetry()` parameters and JSON formatting
- Add QoS levels: Modify publish call parameters
- Add subscriptions: Implement MQTT_EVENT_DATA handler

## Adding New Features

### Adding a New Sensor

Example: Adding a temperature sensor on ADC1_CH2

**Step 1: Create Header** (`include/temperature.h`)
```c
#ifndef TEMPERATURE_H
#define TEMPERATURE_H

#include "esp_err.h"

esp_err_t temperature_init(void);
float temperature_read_celsius(void);
esp_err_t temperature_deinit(void);

#endif
```

**Step 2: Create Implementation** (`src/temperature.c`)
```c
#include "temperature.h"
#include "adc_manager.h"
// ... follow pattern from soil_moisture.c

static adc_cali_handle_t cali_handle = NULL;
static bool initialized = false;

#define TEMP_ADC_CHAN ADC_CHANNEL_2
// ... rest of implementation
```

**Step 3: Update Build** (`src/CMakeLists.txt`)
```cmake
idf_component_register(
    SRCS 
        "main.c"
        # ... existing files ...
        "temperature.c"    # Add this line
```

**Step 4: Integrate in Main** (`src/main.c`)
```c
#include "temperature.h"

// In init_system():
ret = temperature_init();
if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to initialize temperature sensor");
}

// In telemetry_loop():
float temperature = temperature_read_celsius();

// Update mqtt_publisher.h and .c to include temperature in JSON
```

### Adding New Storage

Example: Storing sensor calibration in NVS

**Pattern**: Follow `wifi_credentials.c` structure

```c
// In wifi_credentials.c or new module
#define NVS_KEY_SOIL_CAL_DRY "soil_cal_dry"
#define NVS_KEY_SOIL_CAL_WET "soil_cal_wet"

bool load_soil_calibration(int *dry_mv, int *wet_mv) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    
    int32_t dry, wet;
    err = nvs_get_i32(handle, NVS_KEY_SOIL_CAL_DRY, &dry);
    err |= nvs_get_i32(handle, NVS_KEY_SOIL_CAL_WET, &wet);
    
    nvs_close(handle);
    
    if (err == ESP_OK) {
        *dry_mv = (int)dry;
        *wet_mv = (int)wet;
        return true;
    }
    return false;
}
```

### Adding Deep Sleep

For battery optimization:

**Step 1: Include ESP Power Management**
```c
#include "esp_sleep.h"
```

**Step 2: Replace Telemetry Loop**
```c
void telemetry_loop(void) {
    while (1) {
        // Read sensors
        float voltage = battery_monitor_read_voltage();
        float moisture = soil_moisture_read_percentage();
        
        // Publish
        if (wifi_manager_is_connected() && mqtt_publisher_is_connected()) {
            mqtt_publisher_publish_telemetry(voltage, moisture, device_id_buffer);
            
            // Wait for MQTT to send
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        
        // Enter deep sleep
        ESP_LOGI(TAG, "Entering deep sleep for 5 minutes");
        esp_deep_sleep(5 * 60 * 1000000ULL);  // 5 minutes in microseconds
    }
}
```

**Note**: Device will reboot on wake from deep sleep

## Code Style Guidelines

### Naming Conventions

- **Functions**: `module_verb_noun()` - e.g., `battery_monitor_read_voltage()`
- **Files**: `snake_case.c` - e.g., `wifi_credentials.c`
- **Constants**: `UPPER_SNAKE_CASE` - e.g., `SENSOR_DRY_MV`
- **Types**: `snake_case_t` - e.g., `cali_entry_t`
- **Static vars**: `snake_case` - e.g., `cali_handle`

### Documentation

**Required for**:
- All public functions (in header files)
- All complex functions (in source files)
- All configuration constants
- All module-level comments

**Use Doxygen style**:
```c
/**
 * @brief Short description
 * 
 * Longer description with details about:
 * - What the function does
 * - When to call it
 * - Important constraints
 * 
 * @param param_name Description of parameter
 * @return Description of return value
 * 
 * @note Important information
 * @warning Critical warnings
 */
```

### Error Handling

**Always check return values**:
```c
esp_err_t err = some_function();
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to do something: %s", esp_err_to_name(err));
    return err;
}
```

**Use appropriate log levels**:
- `ESP_LOGE()` - Errors that prevent operation
- `ESP_LOGW()` - Warnings about non-critical issues
- `ESP_LOGI()` - Important information (init, milestones)
- `ESP_LOGD()` - Debug information (verbose)

## Testing Guidelines

### Manual Testing Checklist

- [ ] First boot (no credentials) enters provisioning
- [ ] Provisioning web interface accessible
- [ ] WiFi credentials save and persist after restart
- [ ] Factory reset clears credentials (5 second hold)
- [ ] Battery voltage reads correctly (verify with multimeter)
- [ ] Soil moisture reads 0% in air
- [ ] Soil moisture reads ~100% in water
- [ ] MQTT publishes once per wake (1 hour interval in deep-sleep mode; or every ~5 s under `-DDISABLE_DEEP_SLEEP` for bench testing)
- [ ] Device reconnects after WiFi loss
- [ ] Device handles MQTT broker disconnect

### Serial Monitor Validation

Look for successful initialization sequence on a timer wake (abbreviated; ESP-IDF `wifi:` lines omitted for brevity):
```
I MAIN: === DFR-MoistureTracker Starting ===
I MAIN: Wake from deep sleep - initializing...
I MAIN: Wake cause: Timer
I MAIN: ADC manager initialized
I BATTERY: Battery monitor initialized on ADC1 Channel 0
I SOIL_MOISTURE: Soil moisture sensor initialized on ADC1 Channel 2
I MAIN: OCV: 4.210V (100% SoC)
I WIFI_MGR: WiFi connected successfully
I MQTT_PUB: MQTT connected
I MAIN: === Initialization Complete ===
I MAIN: Publishing telemetry: Battery=4.21V, Moisture=1.4%
I MAIN: Entering deep sleep for 3600 seconds (60 minutes)
```

The `OCV:` line **must appear before** any `wifi:` lines — that's the zero-load sampling property.

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

## Performance Considerations

### Power Consumption

Active-window characteristics (during the ~5 s wake):
- WiFi active: ~80 mA
- MQTT keepalive: ~5 mA average
- ADC readings: ~2 mA for 100 ms
- **Peak**: ~85 mA

With deep sleep (1 hour intervals, current configuration):
- Active: ~5 s × ~85 mA ≈ 0.118 mAh per wake
- Sleep: 3595 s × ~10 µA ≈ 0.010 mAh per hour
- **Average draw**: ~0.13 mAh / hour ≈ ~130 µA
- **Theoretical battery life** (2000 mAh cell, ignoring self-discharge): ~640 days

Real-world life is shorter: LiPo self-discharge (~3 %/month), WiFi retries on weak signal, brownout events, and capacity loss with age all eat into this. A realistic estimate at moderate signal strength is several months to a year on a 2000 mAh cell.

If the cell drops below `BATTERY_LOW_CUTOFF_V` (3.70 V), the firmware skips WiFi entirely on subsequent wakes — sleep current is unchanged but per-wake cost drops to just the ADC sample, extending life on a starving cell.

### Memory Usage

- Heap usage: ~80KB
- Stack usage: ~12KB (main task)
- Static data: ~2KB

## Common Issues and Solutions

### Issue: "ADC manager not initialized"
**Cause**: Sensor init called before adc_manager_init()
**Solution**: Ensure init_system() calls adc_manager_init() first

### Issue: "Failed to create ADC unit" 
**Cause**: Multiple ADC unit creation attempts
**Solution**: Use adc_manager - only one unit instance allowed

### Issue: Sensor reads 0 constantly
**Cause**: Wrong GPIO or disconnected sensor
**Solution**: Check wiring, verify ADC channel matches GPIO

### Issue: WiFi won't connect
**Cause**: Incorrect credentials or out of range
**Solution**: Factory reset and re-provision

### Issue: MQTT not publishing
**Cause**: Broker unreachable or wrong credentials
**Solution**: Check broker URI, verify network connectivity

## Useful Commands

```bash
# Clean build
platformio run --target clean

# Build
platformio run

# Upload
platformio run --target upload

# Monitor
platformio run --target monitor

# Upload + Monitor
platformio run --target upload --target monitor

# Erase flash completely
platformio run --target erase

# Check code size
platformio run --target size

# Build verbose
platformio run -v
```

## Zigbee Build (SP1)

The Zigbee transport is behind the `USE_ZIGBEE` build flag. WiFi/MQTT remains the
default build. There are three relevant envs:

| Env | Transport | Sleep | Use |
|-----|-----------|-------|-----|
| `dfrobot_firebeetle2_esp32c6` | WiFi/MQTT | deep sleep | default, production WiFi |
| `dfrobot_firebeetle2_esp32c6_zigbee` | Zigbee | managed light sleep | production Zigbee |
| `dfrobot_firebeetle2_esp32c6_zigbee_test` | Zigbee | none (stays awake) | bench: keeps USB-JTAG up |

The Zigbee envs inject `sdkconfig.defaults.zigbee` via
`board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS=...` (the only mechanism that
works for a pure-espidf build — `board_build.sdkconfig_defaults` and pioarduino's
`custom_sdkconfig` are no-ops here). This enables the 802.15.4 radio, the Zigbee
end-device role, 4 MB flash, and the `zb_storage`/`zb_fct` partitions.

### Build & flash the Zigbee firmware

```bash
# Production (managed light sleep)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor

# Bench (stays awake, USB-JTAG stays up for re-flashing)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee_test -t upload -t monitor
```

### Power model — managed light sleep

The Zigbee build does **not** use ESP deep sleep. `app_main()` brings up the stack,
joins, then returns; the esp-zigbee task stays alive and uses the SDK's managed
light-sleep (`esp_zb_sleep_enable`). A scheduler alarm (`periodic_report_cb`)
re-samples the sensors and pushes attribute updates every `ZIGBEE_REPORT_INTERVAL_SEC`
(900 s / 15 min). Staying associated avoids the ZigBee end-device aging timeout that
caused stale readings under full deep sleep + rejoin.

> Why not `esp_zb_zcl_report_attr_cmd_req()`? It asserts in the stack for all
> clusters (zcl_general_commands.c:612). Reporting is done with
> `esp_zb_zcl_set_attribute_val()` from the scheduler callback (stack context, so
> **no** `esp_zb_lock_acquire`) plus device-side reporting config.

### Clusters

| Cluster | ID | Attribute | Carries |
|---------|-----|-----------|---------|
| Power Configuration | 0x0001 | 0x0020 BatteryVoltage (100 mV) / 0x0021 BatteryPercentageRemaining (0.5%) | battery |
| Relative Humidity | 0x0405 | 0x0000 MeasuredValue (0.01%, uint16) | soil moisture |

Soil moisture rides the standard Humidity cluster because the SDK's custom Soil
Moisture cluster (0x0408) asserts; both use the same 0.01% uint16 wire format.

### Pairing

1. In zigbee2mqtt, enable `permit_join`.
2. Power-cycle the sensor (or first flash). It auto-runs BDB steering and joins.
3. Install `z2m/dfr_soil_moisture.js` as an external converter and restart zigbee2mqtt.
4. The device appears as `DFR-SoilSensor` (vendor `DFRobot-DIY`) with `soil_moisture`
   and `battery` entities. The modelID matches `zigbeeModel`, so a z2m restart
   re-applies the converter without re-pairing.

### Verification checklist

- [ ] Device joins and appears in zigbee2mqtt without manual interview errors.
- [ ] `soil_moisture` tracks reality (low in air, high in water).
- [ ] `battery` % and voltage are plausible (note: inflated while solar-charging — known limitation).
- [ ] ≥3 consecutive report cycles without a new join logged (rejoin-free; confirms the device stays associated).

### On-device config (SP2)

A short press of the **GPIO7 button** reboots the device into a SoftAP config
portal (`FireBeetle_C6_Prov`, `http://192.168.4.1`) with **no WiFi-station setup** —
the Zigbee stack is not running in this mode, so there is no radio coexistence
issue. From a phone you can:

- **Set Sensor Name** — persisted to NVS (`device_id`), shown on the e-paper, and
  written to the Basic cluster LocationDescription (0x0010). The z2m converter
  surfaces it as the `label` field in every MQTT payload, so Node-RED can identify
  the physical sensor. Max 16 characters.
- **Calibrate Sensor** — the standard dry/wet capture flow with live mV readout.
- **Done – Reboot** — returns immediately to normal Zigbee operation (otherwise the
  portal exits on a 10-minute idle timeout).

The device reboots back into Zigbee on save/reboot and auto-rejoins with its
persisted network credentials.

### Known limitations (SP1)

- Battery SoC reads high during daylight solar charging (terminal sits at charger CV voltage).
- Pairing is auto-steer-on-boot only; button-driven *pairing* arrives in SP3
  (the GPIO7 button is used for the config portal in SP2 — see above).
- The e-paper refreshes after each periodic report, on a dedicated task so the
  ~2 s full refresh runs off the Zigbee stack loop.

## OTA Updates

`src/ota_client.c` adds the Zigbee OTA Upgrade client cluster (0x0019). The device
advertises `manufacturerCode = 0xFEFE`, `imageType = 0x0001`, and a `fileVersion`
packed from the git tag via `OTA_PACK_VERSION` (`include/ota_ids.h`). Zigbee2mqtt
serves images from a GitHub Releases asset; `ota/index.json` is the discovery
index the z2m instance polls.

### One-time bootstrap (per node, over USB)

The OTA firmware uses a **dual-OTA partition table** (`otadata` + `ota_0` + `ota_1`,
1.5 MB each). This layout is incompatible with the old single-app table, so the
first OTA-capable build must be flashed over USB. After that all updates are wireless.

```bash
# Erase old partition table and flash OTA-capable firmware in one step:
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t erase -t upload
```

After flashing, the device will not rejoin automatically — the erase wiped Zigbee
network credentials. Re-pair in zigbee2mqtt (enable `permit_join`, power-cycle the
sensor). This is a one-time step; subsequent updates do not require re-pairing.

### z2m configuration

Add (or extend) the `ota:` block in zigbee2mqtt's `configuration.yaml`:

```yaml
ota:
  # Point at the raw GitHub URL of ota/index.json on master:
  zigbee_ota_override_index_location: "https://raw.githubusercontent.com/<owner>/<repo>/master/ota/index.json"
  # Do not auto-apply updates fleet-wide — roll out manually, one device at a time:
  disable_automatic_update_check: true
```

Replace `<owner>/<repo>` with the actual GitHub organisation and repository name.
The converter (`z2m/dfr_soil_moisture.js`) already declares `ota: ota.zigbeeOTA`,
so no converter change is needed.

### Cutting a release

Tag the commit you want to ship and push the tag. The GitHub Action
(`.github/workflows/release-ota.yml`) handles the rest:

```bash
git tag v1.2.3
git push --tags
```

The action:
1. Derives `FW_VER_MAJOR/MINOR/PATCH` from the tag and injects them into the
   firmware build via `-D` flags, producing a deterministic `FW_VERSION_U32`.
2. Wraps `firmware.bin` into `firmware-v1.2.3.ota` using `tools/make_ota_image.py`.
3. Publishes a GitHub Release with that asset attached.
4. Updates `ota/index.json` (via `tools/update_ota_index.py`) and commits it to
   `master` — z2m will pick it up on its next index poll.

**Version ordering**: `fileVersion` is packed as `0xMMmmppBB` (major, minor, patch,
build). A release tag must produce a strictly larger `fileVersion` than the firmware
currently running on the target nodes — otherwise z2m will not offer the update.
Local dev builds default to `0x00000000`; any tagged release supersedes them.

### Staged rollout / canary

With `disable_automatic_update_check: true`, z2m will not push updates without
operator action. To roll out a new image:

1. In the zigbee2mqtt UI, open **one** device → **OTA** tab → **Update**.
2. Observe progress on the serial monitor (if attached) or in the z2m state:
   - `OTA start -> slot ota_N` — download begins, burst mode activates (rx-on, no
     light sleep, periodic reports paused, 60 s stall watchdog running).
   - Data blocks arrive over the air; the download typically takes a few minutes
     over a good 2.4 GHz link.
   - `OTA complete — rebooting into new image` — device reboots.
   - Device rejoins zigbee2mqtt (watch for the join log) and pushes its first report.
   - `New image confirmed valid (rollback cancelled)` — the bootloader commit is
     done; the update is permanent.
3. Verify readings look correct for the canary node.
4. Repeat step 1–3 for each remaining node.

### Rollback behavior

The bootloader runs with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A freshly
flashed image stays in `ESP_OTA_IMG_PENDING_VERIFY` until `ota_client_mark_valid()`
is called. That call happens only after the device successfully rejoins Zigbee and
delivers at least one telemetry report.

If the new image fails to rejoin and report (crash loop, radio failure, etc.) the
bootloader automatically reverts to the previous slot on the next boot. The canary
step exists specifically to catch a "boots but misbehaves" image before it reaches
the whole fleet.

> **Note**: there is no SoftAP recovery or secure-boot bypass in this version.
> If rollback also fails (both slots bad), re-flash over USB.

### Limitations and notes

- **Slot size**: each OTA slot is `0x180000` = **1.5 MB**. The current application
  image is ~1.25 MB (~83% of the slot). Monitor growth with `pio run -t size` before
  adding large features; exceeding the slot size will abort the OTA at the write
  stage.
- **Sleepy-ED download speed**: the burst-mode logic (rx-on, no light sleep) is
  automatic and required — without it a sleepy end-device download would take hours
  or time out. Do not disable it.
- **Index URL placeholder**: `<owner>/<repo>` in `configuration.yaml` must be
  replaced with the real GitHub org/repo before any device will discover updates.
- **Query interval**: the client polls the OTA server every 30 minutes
  (`OTA_QUERY_INTERVAL_MIN`). After a release is published it may take up to
  30 minutes before z2m marks the device as "update available".

## Resources

- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/)
- [PlatformIO ESP32 Platform](https://docs.platformio.org/en/latest/platforms/espressif32.html)
- [DFRobot FireBeetle 2 ESP32-C6 Wiki](https://wiki.dfrobot.com/SKU_DFR0868_FireBeetle_2_Board_ESP32_C6)
- [MQTT Protocol Specification](https://mqtt.org/mqtt-specification/)

## Contributing

When adding features:
1. Follow existing code structure and style
2. Add comprehensive documentation
3. Update relevant .md files
4. Test thoroughly before committing
5. Keep modules focused and independent
