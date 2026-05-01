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
2. `setup_wifi()` — checks NVS for credentials; enters provisioning (SoftAP + HTTP) if none found
3. `setup_mqtt()` — reads device ID from NVS, constructs topic `zigbee2mqtt/{device_id}`, connects to broker
4. `publish_telemetry_once()` — reads battery voltage + soil moisture, publishes JSON, waits 2s for delivery
5. `enter_deep_sleep(3600)` — timer wakeup after 1 hour; does not return

### ADC Resource Sharing (Critical Constraint)

ESP32-C6 ADC units **cannot be initialized more than once**. `adc_manager` owns the single ADC1 unit handle and must be initialized before any sensor:

- `adc_manager_init()` — called first in `init_system()`
- `adc_manager_get_handle()` — used by both battery monitor and soil moisture
- Adding a third ADC sensor: use `ADC_CHANNEL_2` (or higher) via the same shared handle — do not create a second unit

### Module Responsibilities

| Module | File | Purpose |
|--------|------|---------|
| `adc_manager` | `src/adc_manager.c` | Shared ADC1 handle — must init first |
| `wifi_credentials` | `src/wifi_credentials.c` | NVS read/write for SSID, password, device ID |
| `wifi_manager` | `src/wifi_manager.c` | WiFi STA connection and status |
| `wifi_provisioning` | `src/wifi_provisioning.c` | SoftAP (`FireBeetle_C6_Prov`) + HTTP server at 192.168.4.1 |
| `battery_monitor` | `src/battery_monitor.c` | ADC1 CH0 (GPIO 0) — 2:1 voltage divider |
| `soil_moisture` | `src/soil_moisture.c` | ADC1 CH1 (GPIO 1) — DFRobot capacitive sensor |
| `mqtt_publisher` | `src/mqtt_publisher.c` | MQTT client lifecycle and JSON telemetry |
| `factory_reset` | `src/factory_reset.c` | GPIO 20 button — 5-second hold clears NVS and restarts |

### Key Configuration Constants (`src/main.c`)

```c
#define DEFAULT_DEVICE_ID         "sensor02"   // Fallback if not provisioned
#define DEEP_SLEEP_INTERVAL_SEC   3600          // 1 hour
#define WIFI_TIMEOUT_SEC          30            // Auto-restart if WiFi fails
#define MQTT_WAIT_MS              3000          // Max wait for broker connect
#define PUBLISH_WAIT_MS           2000          // Drain time before sleep
```

### Soil Moisture Calibration (`src/soil_moisture.c`)

```c
#define SENSOR_DRY_MV   2800   // ADC mV reading in open air
#define SENSOR_WET_MV   1200   // ADC mV reading fully submerged
```

See `SOIL_MOISTURE_SETUP.md` for the calibration procedure.

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

On first boot (or after factory reset), device starts SoftAP `FireBeetle_C6_Prov`. Connect to that network and navigate to `http://192.168.4.1` to set SSID, password, and device ID. Credentials persist in NVS across deep sleep cycles.
