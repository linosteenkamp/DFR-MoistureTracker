# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-C6 IoT firmware (C/ESP-IDF via PlatformIO) for a battery-powered soil moisture and battery telemetry sensor. The device wakes from deep sleep hourly, publishes a JSON reading to MQTT, then sleeps again (~10µA sleep vs ~85mA active).

**Target board**: DFRobot FireBeetle 2 ESP32-C6 (`dfrobot_firebeetle2_esp32c6`)

## Build & Flash Commands

```bash
# Build
pio run

# Upload firmware
pio run --target upload

# Serial monitor (115200 baud)
pio run --target monitor

# Upload then monitor in one step
pio run --target upload --target monitor

# Clean build artifacts
pio run --target clean

# Erase all flash (use before re-provisioning in development)
pio run --target erase

# Check firmware size
pio run --target size

# Verbose build output
pio run -v
```

### Zigbee build

WiFi/MQTT is the default. A Zigbee transport variant lives behind the `USE_ZIGBEE`
flag (native `esp_zb_*` API, esp-zigbee-lib 1.6.x):

```bash
# Production Zigbee (managed light sleep, reports every 15 min)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor

# Bench Zigbee (stays awake, USB-JTAG stays up)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee_test -t upload -t monitor
```

The Zigbee env injects `sdkconfig.defaults.zigbee` via `board_build.cmake_extra_args`
(the only sdkconfig mechanism that works for pure-espidf). It uses **managed light
sleep**, not ESP deep sleep — `app_main()` joins then returns; the stack task pushes
periodic reports via a scheduler alarm. Soil rides the standard Humidity cluster
(0x0405); battery on Power Config (0x0001). See "Zigbee Build (SP1)" in
`DEVELOPER_GUIDE.md` for the full pairing/verification flow and the z2m converter
(`z2m/dfr_soil_moisture.js`).

There are no automated tests — validation is manual via serial monitor. See the checklist in `DEVELOPER_GUIDE.md`.

## Credentials Setup (Required Before First Build)

MQTT credentials are **not committed to git**. Create the credentials file from the example:

```bash
cp include/mqtt_credentials.h.example include/mqtt_credentials.h
# Edit include/mqtt_credentials.h with your broker URI, username, and password
```

`include/mqtt_credentials.h` is in `.gitignore` and must never be committed.

## Architecture

### Boot Cycle (Deep Sleep Model)

`app_main()` runs on every wake. Each wake is a full reboot — there is no persistent state in RAM (only NVS and RTC memory survive sleep):

1. `init_system()` — NVS flash, TCP/IP stack, event loop, ADC manager, factory reset GPIO, sensors
2. **Early OCV sample** — `battery_monitor_read_voltage()` runs *before* WiFi energizes (zero-load reading). If cell < `BATTERY_LOW_CUTOFF_V` (3.70 V), draws a one-shot low-battery warning on the e-paper (latched in RTC memory so subsequent wakes skip the redraw) and sleeps without touching WiFi. Otherwise the OCV is cached in `g_cached_battery_v` for step 5.
3. `setup_wifi()` — checks NVS for credentials; enters provisioning (SoftAP + HTTP) if none found
4. `setup_mqtt()` — reads device ID from NVS, constructs topic `zigbee2mqtt/{device_id}`, connects to broker
5. `publish_telemetry_once()` — reads soil moisture, reuses the pre-sampled OCV (not a fresh under-load read), publishes JSON, waits 2s for delivery
6. `enter_deep_sleep(3600)` — timer wakeup after 1 hour; does not return

### ADC Resource Sharing (Critical Constraint)

ESP32-C6 ADC units **cannot be initialized more than once**. `adc_manager` owns the single ADC1 unit handle and must be initialized before any sensor:

- `adc_manager_init()` — called first in `init_system()`
- `adc_manager_get_handle()` — used by both battery monitor and soil moisture
- Adding a third ADC sensor: GPIO4/ADC1_CH4 is **no longer free** (display BUSY). Use `ADC_CHANNEL_1`, `_5`, or `_6` (GPIO1 is the display CS — no longer free either; GPIO5/6 are still free). See `SOIL_MOISTURE_SETUP.md` for the full pinout. Use the shared `adc_manager` handle — do not create a second unit

### Module Responsibilities

| Module | File | Purpose |
|--------|------|---------|
| `adc_manager` | `src/adc_manager.c` | Shared ADC1 handle — must init first |
| `wifi_credentials` | `src/wifi_credentials.c` | NVS read/write for SSID, password, device ID |
| `wifi_manager` | `src/wifi_manager.c` | WiFi STA connection and status |
| `battery_monitor` | `src/battery_monitor.c` | ADC1 CH0 (GPIO 0) — 2:1 voltage divider. Also hosts the pure 11-point LiPo SoC curve (`battery_monitor_v_to_pct`) and brownout-cutoff check (`battery_monitor_is_safe`); both declared in host-safe `include/battery_soc.h` |
| `soil_moisture` | `src/soil_moisture.c` | ADC1 CH2 (GPIO 2) for AOUT, GPIO 3 as switched VCC — sensor is only powered during read |
| `mqtt_publisher` | `src/mqtt_publisher.c` | MQTT client lifecycle and JSON telemetry |
| `soil_calibration` | `src/soil_calibration.c` | NVS-backed runtime dry/wet mV values (namespace `soil_cal`) — sane defaults if missing |
| `config_portal` | `src/config_portal.c` | SoftAP (`FireBeetle_C6_Prov`) + HTTP portal (WiFi, calibration, status, factory reset) |
| `form_parser` | `src/form_parser.c` | URL-encoded form field extraction (pure C, host-testable) |
| `nvs_shim` | `src/nvs_shim_esp.c` | u32 wrapper around ESP NVS for host-testable modules |
| `display` | `src/display.c` | Waveshare 2.13" e-paper driver (SSD1680) + framebuffer + dashboard / portal / low-battery layouts |

### Key Configuration Constants (`src/main.c`)

```c
#define DEFAULT_DEVICE_ID         "sensor02"   // Fallback if not provisioned
#define DEEP_SLEEP_INTERVAL_SEC   3600          // 1 hour
#define WIFI_TIMEOUT_SEC          30            // Auto-restart if WiFi fails
#define MQTT_WAIT_MS              3000          // Max wait for broker connect
#define PUBLISH_WAIT_MS           2000          // Drain time before sleep
```

### Soil Moisture Calibration

Calibration values are stored per device in NVS namespace `soil_cal` (`dry_mv`, `wet_mv`, `cal_ts`) and captured through the config portal. See [CONFIG_PORTAL.md](CONFIG_PORTAL.md). Defaults (`dry_mv=2800`, `wet_mv=0`) are used when NVS has no calibration.

## Adding a New Sensor

Follow the pattern from `battery_monitor.c` / `soil_moisture.c`:

1. Create `include/sensor_name.h` and `src/sensor_name.c`
2. Get the ADC handle via `adc_manager_get_handle()` — do **not** call `adc_unit_new_unit()`
3. Register the new `.c` file in `src/CMakeLists.txt` under `SRCS`
4. Call `sensor_name_init()` in `init_system()` after `adc_manager_init()`
5. Call `sensor_name_read_*()` in `publish_telemetry_once()`
6. Extend `mqtt_publisher_publish_telemetry()` signature and JSON output

## Naming Conventions

- Functions: `module_verb_noun()` — e.g., `battery_monitor_read_voltage()`
- Constants: `UPPER_SNAKE_CASE`
- Types: `snake_case_t`
- Log tags: short uppercase string, e.g., `static const char *TAG = "BATTERY";`

## Flash Partitions

Defined in `partitions.csv`:
- `nvs` (24KB) — WiFi credentials, device ID
- `phy_init` (4KB) — RF calibration
- `factory` (2.5MB) — application firmware
- `storage` (16KB) — reserved NVS namespace for future sensor calibration data

## Provisioning Flow

On first boot (no WiFi credentials in NVS) **or** when the GPIO7 button is pressed during deep sleep, the device opens SoftAP `FireBeetle_C6_Prov` and serves a config portal at `http://192.168.4.1`. See [CONFIG_PORTAL.md](CONFIG_PORTAL.md) for full documentation.
