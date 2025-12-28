# DFR-MoistureTracker

**ESP32-C6 Environmental Monitoring System**

Battery-powered ESP32-C6 IoT device with WiFi provisioning, soil moisture monitoring, battery monitoring, and MQTT telemetry publishing. Optimized for long battery life using deep sleep mode.

## Features

- **WiFi Provisioning**: SoftAP + web interface for easy WiFi configuration
- **Device ID Configuration**: Customize MQTT topic during provisioning
- **Factory Reset Button**: GPIO 20 long press (5 seconds) to reset credentials
- **Soil Moisture Monitoring**: DFRobot capacitive sensor with calibration support
- **Battery Monitoring**: ADC-based voltage reading with averaging
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
- **Soil Moisture Sensor**: GPIO 1 (ADC1 Channel 1) - DFRobot Capacitive Sensor
- **Factory Reset Button**: GPIO 20 to GND (internal pull-up enabled)

### Wiring Diagram

```
FireBeetle 2 ESP32-C6
┌─────────────────────┐
│                     │
│  GPIO 0 (ADC1_CH0) ─┼─→ Battery Voltage (2:1 divider)
│  GPIO 1 (ADC1_CH1) ─┼─→ Soil Moisture Sensor (AOUT)
│  GPIO 20           ─┼─→ Factory Reset Button → GND
│                     │
│  3V3               ─┼─→ Sensor VCC (Red)
│  GND               ─┼─→ Sensor GND (Black), Button
│                     │
└─────────────────────┘
```

### Factory Reset Button Wiring

Connect a momentary push button between:
- **Pin 1**: GPIO 20 (on FireBeetle expansion header)
- **Pin 2**: GND (any ground pin)

The internal pull-up resistor keeps GPIO 20 HIGH (~3.3V) when button is not pressed.
Pressing the button connects GPIO 20 to GND (reads LOW).

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
To reset the device and clear all stored credentials:

1. **Press and hold** the button connected to **GPIO 20**
2. **Hold for 5 seconds** (device will log countdown in serial monitor)
3. Device will:
   - Clear WiFi SSID, password, and device ID from NVS
   - Display "Factory reset triggered!" message
   - Restart automatically
   - Boot into provisioning mode

**Serial Monitor Output Example:**
```
I (12345) FACTORY_RESET: Button pressed - hold for 5 seconds to factory reset
I (13345) FACTORY_RESET: Hold for 4 more seconds...
I (14345) FACTORY_RESET: Hold for 3 more seconds...
I (15345) FACTORY_RESET: Hold for 2 more seconds...
I (16345) FACTORY_RESET: Hold for 1 more seconds...
W (17345) FACTORY_RESET: Factory reset triggered!
W (17345) FACTORY_RESET: Clearing WiFi credentials...
W (17350) FACTORY_RESET: Restarting in 2 seconds...
```

**Note**: Releasing the button before 5 seconds will cancel the reset.

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

Press and hold button on **GPIO 20** for **5 seconds**:
- Clears all WiFi credentials and device ID
- Restarts into provisioning mode
- Progress shown in serial monitor

## Configuration

Edit [src/main.c](src/main.c):

```c
#define MQTT_BROKER_URI      "mqtt://499.steenkamps.org"
#define MQTT_USERNAME        "mqtt"
#define MQTT_PASSWORD        "2tVEquV2aaUm"
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

### Soil Moisture Sensor Calibration

Edit [src/soil_moisture.c](src/soil_moisture.c):

```c
#define SOIL_ADC_CHAN         ADC_CHANNEL_1  // GPIO1
#define SENSOR_DRY_MV         2800           // Voltage in air (calibrate)
#define SENSOR_WET_MV         1200           // Voltage in water (calibrate)
```

See [SOIL_MOISTURE_SETUP.md](SOIL_MOISTURE_SETUP.md) for calibration procedure.

## Architecture

Modular design following SOLID principles:

- **adc_manager**: Shared ADC1 unit management for multiple sensors
- **wifi_credentials**: NVS storage management
- **wifi_manager**: WiFi STA connection handling
- **wifi_provisioning**: SoftAP + HTTP provisioning server
- **battery_monitor**: ADC voltage reading for battery
- **soil_moisture**: Capacitive soil moisture sensor interface
- **mqtt_publisher**: MQTT client and telemetry publishing
- **factory_reset**: GPIO button monitoring for factory reset
- **main**: Application orchestration and telemetry loop

### ADC Resource Sharing

Both battery monitor and soil moisture sensor share ADC1 through the ADC manager:
- Single ADC unit handle prevents resource conflicts
- Multiple channels (CH0, CH1) configured independently
- Calibration handles managed per channel
- Efficient multi-sensor ADC usage

## Documentation

- **[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)** - Complete developer documentation for maintenance and extension
- **[SOIL_MOISTURE_SETUP.md](SOIL_MOISTURE_SETUP.md)** - Soil moisture sensor wiring, calibration, and troubleshooting
- **[BATTERY_MONITOR.md](BATTERY_MONITOR.md)** - Battery monitoring system, voltage interpretation, and technical details
- **[FACTORY_RESET.md](FACTORY_RESET.md)** - Factory reset button setup, usage, and configuration
- **[WIFI_PROVISIONING.md](WIFI_PROVISIONING.md)** - WiFi provisioning process, web interface, and reset procedures
- **[PARTITIONS.md](PARTITIONS.md)** - Flash memory partition layout, NVS storage, and customization guide

## Quick Reference

### Pin Assignments
| Function | GPIO | ADC Channel | Notes |
|----------|------|-------------|-------|
| Battery Monitor | GPIO 0 | ADC1_CH0 | Built-in voltage divider (2:1) |
| Soil Moisture | GPIO 1 | ADC1_CH1 | DFRobot capacitive sensor |
| Factory Reset | GPIO 20 | - | Button to GND, 5s hold |

### Default Values
| Setting | Value | Location |
|---------|-------|----------|
| MQTT Broker | `mqtt://499.steenkamps.org` | [src/main.c](src/main.c) |
| Deep Sleep Interval | 3600 seconds (1 hour) | [src/main.c](src/main.c) |
| WiFi AP Name | `FireBeetle_C6_Prov` | Provisioning mode |
| Default Device ID | `sensor02` | [src/main.c](src/main.c) |
| Soil Dry Value | 2800 mV | [src/soil_moisture.c](src/soil_moisture.c) |
| Soil Wet Value | 1200 mV | [src/soil_moisture.c](src/soil_moisture.c) |

## Troubleshooting

**WiFi connection fails:**
- Wait 30 seconds - device auto-resets to provisioning mode
- Or use factory reset button (GPIO 20, hold 5 seconds)

**MQTT not publishing:**
- Check broker URI and credentials in [src/main.c](src/main.c)
- Verify WiFi connection in serial monitor
- Ensure MQTT broker is accessible

**Soil moisture sensor reads 0% constantly:**
- Check wiring connections (VCC, GND, AOUT → GPIO1)
- Verify sensor has 3.3V power
- Check ADC initialization in serial logs

**Soil moisture readings inaccurate:**
- Perform calibration procedure (see [SOIL_MOISTURE_SETUP.md](SOIL_MOISTURE_SETUP.md))
- Update `SENSOR_DRY_MV` and `SENSOR_WET_MV` values
- Clean sensor probe

**Factory reset button not responding:**
- Check GPIO 20 connection to GND
- Must hold for full 5 seconds
- Watch serial monitor for countdown progress

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