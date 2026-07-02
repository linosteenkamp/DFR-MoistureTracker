# DFR-MoistureTracker

**ESP32-C6 battery-powered soil-moisture + battery telemetry sensor**

A FireBeetle 2 ESP32-C6 device that wakes periodically, reads soil moisture and
battery state, publishes a reading, and goes back to low-power sleep. It ships in
two transport flavours from the same codebase:

| Transport | Build env | How it reports | Sleep model |
|-----------|-----------|----------------|-------------|
| **WiFi + MQTT** (default) | `dfrobot_firebeetle2_esp32c6` | JSON to an MQTT broker (`zigbee2mqtt/{device_id}`) | ESP **deep sleep**, full reboot each wake (hourly) |
| **Zigbee** | `dfrobot_firebeetle2_esp32c6_zigbee` | Native Zigbee clusters to a zigbee2mqtt coordinator | **Managed light sleep**, periodic report (15 min) |

The Zigbee build additionally supports **over-the-air firmware updates** via
zigbee2mqtt. See [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md) for the full Zigbee +
OTA documentation.

## Features

- **Soil moisture monitoring** — DFRobot capacitive sensor, runtime-calibrated
- **Battery monitoring** — ADC voltage reading + LiPo state-of-charge curve, with
  a zero-load (pre-radio) sample and a low-battery cutoff
- **E-paper display** — Waveshare 2.13" (SSD1680) shows telemetry / portal / low-
  battery screens — see [DISPLAY.md](DISPLAY.md)
- **Config portal** — SoftAP web UI for calibration, status, and (WiFi build) WiFi
  setup + factory reset — see [CONFIG_PORTAL.md](CONFIG_PORTAL.md)
- **GPIO7 button** — press during sleep to open the config portal
- **Low-power sleep** — deep sleep (WiFi) or managed light sleep (Zigbee)
- **Zigbee OTA** — CI-built images delivered through zigbee2mqtt (Zigbee build)
- **Shared ADC manager** — single ADC1 unit shared safely across sensors
- **Host-testable core** — pure-C modules with a `native` unit-test suite

## Transports

### WiFi + MQTT (default — `pio run`)

Each wake is a full reboot (no RAM state survives; only NVS + RTC memory do):

1. Init NVS, ADC manager, sensors
2. Zero-load battery sample *before* WiFi energizes; if the cell is below the
   low-battery cutoff, draw a one-shot warning and sleep without WiFi
3. WiFi (provisioning SoftAP on first boot if no credentials)
4. Connect to the MQTT broker
5. Read soil + reuse the pre-sampled battery voltage, publish JSON, drain, sleep

### Zigbee (`pio run -e dfrobot_firebeetle2_esp32c6_zigbee`)

`app_main()` joins the Zigbee network and returns; the stack task pushes periodic
reports (every `ZIGBEE_REPORT_INTERVAL_SEC`, default 15 min) using managed light
sleep between polls. Soil moisture rides the standard **Relative Humidity** cluster
(0x0405), battery on **Power Configuration** (0x0001), and the device-set sensor
name is exposed via Basic **LocationDescription** (0x0010) → surfaced by the z2m
converter (`z2m/dfr_soil_moisture.js`) as the `label` field. A short press of the
GPIO7 button reboots into the SoftAP config portal (calibration + sensor name),
then back into Zigbee.

There is a bench variant, `dfrobot_firebeetle2_esp32c6_zigbee_test`, that stays
awake (no light sleep) so the USB-Serial-JTAG link stays up for debugging.

## Power Management

- **WiFi deep sleep**: ~85 mA active for a few seconds per wake, ~10 µA asleep.
- **Zigbee managed light sleep**: radio sleeps between parent polls; far lower duty
  cycle than keeping WiFi up.

Actual battery life depends heavily on report interval, link quality, and whether a
solar panel is attached. See [BATTERY_MONITOR.md](BATTERY_MONITOR.md) for the SoC
curve and cutoff behaviour. Intervals are tunable (see [Configuration](#configuration)).

## Hardware

- **Board**: DFRobot FireBeetle 2 ESP32-C6 (`dfrobot_firebeetle2_esp32c6`)
- **Battery monitoring**: GPIO 0 (ADC1_CH0), 2:1 voltage divider
- **Soil moisture AOUT**: GPIO 2 (ADC1_CH2) — DFRobot capacitive sensor analog (yellow)
- **Soil moisture VCC**: GPIO 3 — sensor power, switched by firmware (red)
- **Config portal button**: GPIO 7 → GND
- **E-paper display**: SPI (GPIO 1 = CS, GPIO 4 = BUSY, plus the SPI bus) — see [DISPLAY.md](DISPLAY.md)

```
FireBeetle 2 ESP32-C6
┌─────────────────────┐
│  GPIO 0 (ADC1_CH0) ─┼─→ Battery Voltage (2:1 divider)
│  GPIO 2 (ADC1_CH2) ─┼─→ Soil Moisture AOUT (Yellow)
│  GPIO 3            ─┼─→ Soil Moisture VCC (Red) — switched
│  GPIO 7            ─┼─→ Config Portal Button → GND
│  GND               ─┼─→ Sensor GND (Black), Button
└─────────────────────┘
```

> **ADC note**: ESP32-C6 ADC units can't be initialized twice, so all sensors share
> the one `adc_manager` ADC1 handle. GPIO 1 (display CS) and GPIO 4 (display BUSY)
> are taken; free ADC1 channels for a third sensor are CH1/CH5/CH6 (GPIO 1/5/6) —
> see [SOIL_MOISTURE_SETUP.md](SOIL_MOISTURE_SETUP.md).

## Getting Started

### 1. MQTT credentials (required before first build)

Credentials are **not** committed. Create the file from the template:

```shell
cp include/mqtt_credentials.h.example include/mqtt_credentials.h
# edit include/mqtt_credentials.h: broker URI, username, password, topic prefix
```

`include/mqtt_credentials.h` is gitignored and must never be committed. (The Zigbee
build still includes it to compile, but does not use the broker — it talks Zigbee.)

### 2. Build & flash

```shell
# WiFi/MQTT build (default)
pio run                                   # build
pio run -t upload                         # flash
pio run -t upload -t monitor              # flash + serial monitor

# Zigbee build (production: managed light sleep, reports every 15 min)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor

# Zigbee bench build (stays awake, USB-JTAG up)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee_test -t upload -t monitor

# Host unit tests
pio test -e native
```

### 3. First-time setup

- **WiFi build**: boots into provisioning (no saved credentials). Join the open AP
  **`FireBeetle_C6_Prov`**, browse to **http://192.168.4.1**, enter WiFi SSID/password
  and a device ID (MQTT topic becomes `zigbee2mqtt/{device_id}`), submit, reboot.
- **Zigbee build**: open zigbee2mqtt for joining, install the converter
  (`z2m/dfr_soil_moisture.js`), and pair the device. See [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).

## Telemetry

### WiFi + MQTT

Published to `zigbee2mqtt/{device_id}`:

```json
{ "battery": 4.15, "soil_moisture": 67.5, "device": "moisture01" }
```

- `battery` — battery voltage (V)
- `soil_moisture` — 0–100 % (0 = dry, 100 = wet)
- `device` — device ID set during provisioning (default `moisture01`)

### Zigbee

zigbee2mqtt publishes (via the converter) battery %, battery voltage,
`soil_moisture` %, and `label` (the device-set sensor name) for the joined device.

## OTA Updates (Zigbee build)

The Zigbee build updates over the air through zigbee2mqtt. A GitHub Actions workflow
(`.github/workflows/release-ota.yml`) builds the firmware on a `v*` tag, wraps it
into a `.ota` image, attaches it to a GitHub Release, and updates `ota/index.json`;
zigbee2mqtt reads that index over HTTPS and delivers the image to the device, which
writes it to the inactive OTA slot, reboots, and self-validates (with bootloader
rollback if the new image fails to rejoin). Full procedure — cutting a release,
staged rollout, rollback, and the ~3 h sleepy-device transfer time — is in
[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).

## Configuration

**Credentials** live in `include/mqtt_credentials.h` (from the `.example`):
`MQTT_BROKER_URI`, `MQTT_USERNAME`, `MQTT_PASSWORD`, `MQTT_TOPIC_PREFIX`.

**Tunables** live in [src/main.c](src/main.c):

```c
#define DEFAULT_DEVICE_ID           "moisture01"  // fallback if not provisioned
#define DEEP_SLEEP_INTERVAL_SEC     3600          // WiFi deep-sleep wake interval (1 h)
#define ZIGBEE_REPORT_INTERVAL_SEC  900           // Zigbee report interval (15 min)
#define WIFI_TIMEOUT_SEC            30            // auto-restart if WiFi fails
#define TEST_PUBLISH_INTERVAL_MS    5000          // WiFi test-mode re-publish cadence
```

**Calibration** is captured at runtime via the config portal (NVS namespace
`soil_cal`; defaults dry = 2800 mV, wet = 0 mV) — no source edits — see
[CONFIG_PORTAL.md](CONFIG_PORTAL.md).

### Disabling sleep for testing

Build with `-DDISABLE_DEEP_SLEEP` to keep the device awake and re-publish every
`TEST_PUBLISH_INTERVAL_MS` (WiFi) / keep the radio up (Zigbee). For WiFi, uncomment
the `build_flags` line under `[env:dfrobot_firebeetle2_esp32c6]` in
[platformio.ini](platformio.ini); for Zigbee, use the `…_zigbee_test` env.

## Architecture

Modular C with host-testable cores. Key modules:

| Module | Purpose |
|--------|---------|
| `adc_manager` | Shared ADC1 unit handle — initialized first |
| `battery_monitor` | ADC1_CH0 voltage + LiPo SoC curve + low-battery cutoff (`battery_soc.h`) |
| `soil_moisture` | ADC1_CH2 read with switched VCC (GPIO 3) |
| `soil_calibration` | NVS-backed dry/wet mV calibration |
| `display` | SSD1680 e-paper driver + dashboard/portal/low-battery layouts |
| `config_portal` / `form_parser` | SoftAP HTTP portal + URL-encoded form parsing |
| `wifi_credentials` / `wifi_manager` | NVS credential storage + WiFi STA (WiFi build) |
| `mqtt_publisher` | MQTT client + JSON telemetry (WiFi build) |
| `zigbee_reporter` / `zigbee_encode` | Zigbee end-device, clusters, reporting (Zigbee build) |
| `ota_client` | Zigbee OTA Upgrade client (Zigbee build) |
| `nvs_shim` | Host-testable wrapper over ESP NVS |
| `main` | Boot orchestration for both transports |

## Documentation

- **[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)** — maintenance/extension, Zigbee build, OTA procedure
- **[CONFIG_PORTAL.md](CONFIG_PORTAL.md)** — config portal: WiFi/calibration/status/factory reset
- **[DISPLAY.md](DISPLAY.md)** — e-paper wiring, layouts, asset regeneration
- **[SOIL_MOISTURE_SETUP.md](SOIL_MOISTURE_SETUP.md)** — soil sensor wiring & troubleshooting
- **[BATTERY_MONITOR.md](BATTERY_MONITOR.md)** — battery monitoring, SoC curve, cutoff
- **[PARTITIONS.md](PARTITIONS.md)** — flash partition layout (incl. dual-OTA), NVS

## Quick Reference

### Pin assignments
| Function | GPIO | ADC | Notes |
|----------|------|-----|-------|
| Battery monitor | GPIO 0 | ADC1_CH0 | 2:1 voltage divider |
| Soil moisture AOUT | GPIO 2 | ADC1_CH2 | Capacitive sensor analog signal |
| Soil moisture VCC | GPIO 3 | — | Switched power (HIGH during read, LOW in sleep) |
| Config portal button | GPIO 7 | — | Button → GND, press during sleep |
| Display CS / BUSY | GPIO 1 / 4 | — | Waveshare e-paper (SPI) |

### Build environments
| Env | Transport | Notes |
|-----|-----------|-------|
| `dfrobot_firebeetle2_esp32c6` | WiFi + MQTT | default (`pio run`), deep sleep |
| `dfrobot_firebeetle2_esp32c6_zigbee` | Zigbee | production, managed light sleep, OTA |
| `dfrobot_firebeetle2_esp32c6_zigbee_test` | Zigbee | bench, stays awake |
| `native` | — | host unit tests |

### Defaults
| Setting | Value |
|---------|-------|
| Default device ID | `moisture01` |
| Deep-sleep interval (WiFi) | 3600 s (1 h) |
| Zigbee report interval | 900 s (15 min) |
| WiFi provisioning AP | `FireBeetle_C6_Prov` |
| Soil dry / wet defaults | 2800 mV / 0 mV (portal-captured) |

## Troubleshooting

- **WiFi connection fails** — device auto-resets to provisioning after ~30 s; or
  factory-reset via the portal (see [CONFIG_PORTAL.md](CONFIG_PORTAL.md)).
- **MQTT not publishing** — check `include/mqtt_credentials.h`, verify WiFi in the
  serial monitor, confirm the broker is reachable.
- **Soil reads 0 %** — check wiring (VCC GPIO 3, AOUT GPIO 2, GND), confirm the
  sensor powers during read, check ADC init in the logs.
- **Readings inaccurate** — recalibrate via the portal; clean the probe.
- **Config portal won't open** — press GPIO 7 during sleep and wait for the SoftAP.
- **Zigbee won't pair / update** — see [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).

## License

Provided as-is for educational and development purposes.
