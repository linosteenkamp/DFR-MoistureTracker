# DFR-MoistureTracker

**ESP32-C6 Environmental Monitoring System**

Battery-powered ESP32-C6 IoT device with WiFi provisioning, soil moisture monitoring, battery monitoring, and MQTT telemetry publishing. Optimized for long battery life using deep sleep mode.

## Features

- **WiFi Provisioning**: SoftAP + web interface for easy WiFi configuration
- **Device ID Configuration**: Customize MQTT topic during provisioning
- **Config Portal**: SoftAP web UI for WiFi setup, sensor calibration, and factory reset — see [CONFIG_PORTAL.md](CONFIG_PORTAL.md)
- **Soil Moisture Monitoring**: DFRobot capacitive sensor with calibration support
- **Battery Monitoring**: ADC-based voltage reading with averaging
- **E-paper Display**: Waveshare 2.13" e-paper hat shows telemetry and portal info — see [DISPLAY.md](DISPLAY.md)
- **MQTT Publishing**: JSON telemetry to configurable topic
- **Deep Sleep Mode**: Wakes hourly to publish telemetry, dramatically extending battery life
- **Shared ADC Manager**: Efficient multi-sensor ADC resource management
- **NVS Storage**: Persistent credential and configuration storage
- **SOLID Architecture**: Modular codebase with clear separation of concerns

## Power Management

### Deep Sleep Operation

The device uses ESP32's deep sleep mode for maximum battery efficiency:

- **Active Time**: ~5 seconds per wake cycle
- **Sleep Time**: 3,595 seconds (1 hour intervals)
- **Active Current**: ~85mA (WiFi + sensors)
- **Sleep Current**: ~10µA
- **Average Current**: ~1.7mA
- **Battery Life**: 
  - 2000mAh battery ≈ **49 days** continuous operation
  - With deep sleep ≈ **2+ months** on battery

### Wake Cycle

1. Device wakes from deep sleep (full reboot)
2. Reconnects to WiFi
3. Connects to MQTT broker
4. Reads sensors (battery + soil moisture)
5. Publishes telemetry
6. Enters deep sleep for 1 hour
7. Repeat

**Note**: Deep sleep interval configurable via `DEEP_SLEEP_INTERVAL_SEC` in [src/main.c](src/main.c)

## Hardware

- **Board**: DFRobot FireBeetle 2 ESP32-C6
- **Battery Monitoring**: GPIO 0 (ADC1 Channel 0) with 2:1 voltage divider
- **Soil Moisture AOUT**: GPIO 2 (ADC1 Channel 2) — DFRobot Capacitive Sensor analog signal (yellow)
- **Soil Moisture VCC**: GPIO 3 — sensor power, switched by firmware (red)
- **Config Portal Button**: GPIO 7 to GND — press during deep sleep to wake into portal mode

### Wiring Diagram

```
FireBeetle 2 ESP32-C6
┌─────────────────────┐
│                     │
│  GPIO 0 (ADC1_CH0) ─┼─→ Battery Voltage (2:1 divider)
│  GPIO 2 (ADC1_CH2) ─┼─→ Soil Moisture Sensor AOUT (Yellow)
│  GPIO 3            ─┼─→ Soil Moisture Sensor VCC (Red) — switched
│  GPIO 7            ─┼─→ Config Portal Button → GND
│                     │
│  GND               ─┼─→ Sensor GND (Black), Button
│                     │
└─────────────────────┘
```

### Config Portal Button Wiring

Connect a momentary push button between:
- **Pin 1**: GPIO 7 (on FireBeetle expansion header)
- **Pin 2**: GND (any ground pin)

Press during deep sleep to wake the device into portal mode. See [CONFIG_PORTAL.md](CONFIG_PORTAL.md) for details.

## Getting Started

### Build and Upload

```shell
# Build project
$ pio run

# Upload firmware
$ pio run --target upload

# Monitor serial output
$ pio run --target monitor
```

### First-Time Setup (Provisioning)

1. Device boots into provisioning mode (no saved credentials)
2. Connect to WiFi AP: **FireBeetle_C6_Prov** (open network)
3. Navigate to: **http://192.168.4.1**
4. Enter:
   - WiFi SSID
   - WiFi Password
   - Device ID (for MQTT topic: `zigbee2mqtt/{device_id}`)
5. Submit - device saves credentials and restarts
6. Device connects to your WiFi and starts publishing telemetry

## MQTT Telemetry

After provisioning and connecting:
- Wakes from deep sleep every hour
- Connects to configured MQTT broker using saved credentials
- Publishes battery voltage and soil moisture to `zigbee2mqtt/{device_id}`
- JSON format: `{"battery": 4.22, "soil_moisture": 65.3, "device": "{device_id}"}`
- Enters deep sleep to conserve battery

### Example MQTT Message

```json
{
  "battery": 4.15,
  "soil_moisture": 67.5,
  "device": "sensor02"
}
```

**Fields:**
- `battery`: Battery voltage in volts (V)
- `soil_moisture`: Soil moisture percentage 0-100% (0=dry, 100=wet)
- `device`: Device identifier set during provisioning

**Publishing Interval:**
- Default: Every 1 hour (3600 seconds)
- Configurable in [src/main.c](src/main.c) via `DEEP_SLEEP_INTERVAL_SEC`
- Lower intervals = shorter battery life, more frequent data

### Factory Reset

Factory reset is available through the config portal at `http://192.168.4.1/factory-reset`. See [CONFIG_PORTAL.md](CONFIG_PORTAL.md).

## Configuration

Edit [src/main.c](src/main.c):

```c
#define MQTT_BROKER_URI      "mqtt://499.steenkamps.org"
#define MQTT_USERNAME        "mqtt"
#define MQTT_PASSWORD        "password"
#define DEFAULT_DEVICE_ID    "sensor02"
#define DEEP_SLEEP_INTERVAL_SEC  3600  // Wake every 1 hour (3600 seconds)
```

**Deep Sleep Interval Options:**
- 300 seconds = 5 minutes (testing/high frequency monitoring)
- 900 seconds = 15 minutes (frequent updates)
- 1800 seconds = 30 minutes (moderate frequency)
- 3600 seconds = 1 hour (default - good balance)
- 7200 seconds = 2 hours (extended battery life)
- 14400 seconds = 4 hours (maximum battery life)

### Disabling Deep Sleep for Testing

For local development you can disable deep sleep entirely so the device stays awake and re-publishes telemetry every few seconds — keeping WiFi and MQTT alive between reads.

Uncomment the build flag in [platformio.ini](platformio.ini):

```ini
[env:dfrobot_firebeetle2_esp32c6]
board = dfrobot_firebeetle2_esp32c6
build_flags = -DDISABLE_DEEP_SLEEP
```

Then rebuild and flash:

```shell
$ pio run --target upload --target monitor
```

Behavior with the flag set:
- Full init runs once (WiFi → MQTT → first publish)
- Device then loops `publish_telemetry_once()` every `TEST_PUBLISH_INTERVAL_MS` (default 5000 ms, defined in [src/main.c](src/main.c))
- Config portal (GPIO7 button or first boot) still works
- No "Entering deep sleep" log line, no reboot between cycles

Re-comment the line and re-flash to restore production deep-sleep behavior. The flag is orthogonal to `DEEP_SLEEP_INTERVAL_SEC` — both can be tuned independently.

### Soil Moisture Sensor Calibration

Calibration is captured at runtime through the config portal — no source edits required. See [CONFIG_PORTAL.md](CONFIG_PORTAL.md) for the procedure.

## Architecture

Modular design following SOLID principles:

- **adc_manager**: Shared ADC1 unit management for multiple sensors
- **wifi_credentials**: NVS storage management
- **wifi_manager**: WiFi STA connection handling
- **config_portal**: SoftAP + HTTP portal (WiFi, calibration, status, factory reset) — see [CONFIG_PORTAL.md](CONFIG_PORTAL.md)
- **battery_monitor**: ADC voltage reading for battery
- **soil_moisture**: Capacitive soil moisture sensor interface
- **soil_calibration**: NVS-backed runtime dry/wet mV calibration values
- **mqtt_publisher**: MQTT client and telemetry publishing
- **main**: Application orchestration and telemetry loop

### ADC Resource Sharing

Both battery monitor and soil moisture sensor share ADC1 through the ADC manager:
- Single ADC unit handle prevents resource conflicts
- Multiple channels (CH0, CH1) configured independently
- Calibration handles managed per channel
- Efficient multi-sensor ADC usage

## Documentation

- **[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)** - Complete developer documentation for maintenance and extension
- **[CONFIG_PORTAL.md](CONFIG_PORTAL.md)** - Config portal: WiFi setup, calibration, status, and factory reset
- **[DISPLAY.md](DISPLAY.md)** - E-paper display: hardware wiring, telemetry/portal layouts, asset regeneration
- **[SOIL_MOISTURE_SETUP.md](SOIL_MOISTURE_SETUP.md)** - Soil moisture sensor wiring and troubleshooting
- **[BATTERY_MONITOR.md](BATTERY_MONITOR.md)** - Battery monitoring system, voltage interpretation, and technical details
- **[PARTITIONS.md](PARTITIONS.md)** - Flash memory partition layout, NVS storage, and customization guide

## Quick Reference

### Pin Assignments
| Function | GPIO | ADC Channel | Notes |
|----------|------|-------------|-------|
| Battery Monitor | GPIO 0 | ADC1_CH0 | Built-in voltage divider (2:1) |
| Soil Moisture AOUT | GPIO 2 | ADC1_CH2 | DFRobot capacitive sensor analog signal |
| Soil Moisture VCC | GPIO 3 | - | Switched power — HIGH during read, held LOW in sleep |
| Config Portal Button | GPIO 7 | - | Button to GND — press during sleep to open portal |

### Default Values
| Setting | Value | Location |
|---------|-------|----------|
| MQTT Broker | `mqtt://499.steenkamps.org` | [src/main.c](src/main.c) |
| Deep Sleep Interval | 3600 seconds (1 hour) | [src/main.c](src/main.c) |
| WiFi AP Name | `FireBeetle_C6_Prov` | Provisioning mode |
| Default Device ID | `sensor02` | [src/main.c](src/main.c) |
| Soil Dry Default | 2800 mV | NVS `soil_cal` (portal-captured) |
| Soil Wet Default | 0 mV | NVS `soil_cal` (portal-captured) |

## Troubleshooting

**WiFi connection fails:**
- Wait 30 seconds - device auto-resets to provisioning mode
- Or use the config portal factory reset page — see [CONFIG_PORTAL.md](CONFIG_PORTAL.md)

**MQTT not publishing:**
- Check broker URI and credentials in [src/main.c](src/main.c)
- Verify WiFi connection in serial monitor
- Ensure MQTT broker is accessible

**Soil moisture sensor reads 0% constantly:**
- Check wiring connections (VCC, GND, AOUT → GPIO1)
- Verify sensor has 3.3V power
- Check ADC initialization in serial logs

**Soil moisture readings inaccurate:**
- Recalibrate via the config portal (see [CONFIG_PORTAL.md](CONFIG_PORTAL.md))
- Clean sensor probe

**Config portal not opening:**
- Press GPIO7 button during deep sleep and wait a few seconds for the SoftAP to come up
- Factory reset is available at `http://192.168.4.1/factory-reset` once connected

**ADC initialization fails:**
- Check for GPIO conflicts with other peripherals
- Verify ADC manager initialization in logs
- Ensure only one ADC unit per peripheral

## Serial Monitor Output

Typical startup sequence with deep sleep:

```
I (123) MAIN: === DFR-MoistureTracker Starting ===
I (125) MAIN: Wake from deep sleep - initializing...
I (130) MAIN: Wake cause: Timer
I (456) MAIN: Initializing system infrastructure
I (789) MAIN: Initializing ADC manager...
I (800) ADC_MGR: ADC manager initialized
I (850) BATTERY: Battery monitor initialized on ADC1 Channel 0
I (900) SOIL_MOISTURE: Soil moisture sensor initialized on ADC1 Channel 1
I (950) MAIN: WiFi connected successfully
I (1000) MQTT_PUB: MQTT connected
I (1050) MAIN: === Initialization Complete ===
I (1100) BATTERY: Raw ADC: 2048, Pin voltage: 1500 mV, Battery: 3.00 V
I (1150) SOIL_MOISTURE: Moisture: 67.5% (1.850 V, 1850 mV)
I (1200) MQTT_PUB: Publishing: {"battery":3.00,"soil_moisture":67.5,"device":"sensor02"}
I (3200) MAIN: ========================================
I (3201) MAIN: Entering deep sleep for 3600 seconds (60 minutes)
I (3202) MAIN: Device will wake and publish again at next interval
I (3203) MAIN: ========================================
```

**Note**: After deep sleep message, device goes to sleep. Serial output resumes on next wake.

## License

This project is provided as-is for educational and development purposes.
