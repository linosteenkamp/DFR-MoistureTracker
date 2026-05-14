# DFRobot Capacitive Soil Moisture Sensor 2 - Setup Guide

## Hardware Connection

### Wiring to FireBeetle 2 ESP32-C6

Connect the DFRobot Waterproof Capacitive Soil Moisture Sensor 2 to your ESP32-C6:

| Sensor Pin | ESP32-C6 Pin | Description |
|------------|--------------|-------------|
| VCC (Red)  | GPIO3        | Power switched by firmware — HIGH during read, LOW (held) during deep sleep |
| GND (Black)| GND          | Ground |
| AOUT (Yellow) | GPIO2 (ADC1_CH2) | Analog output signal |

**Why GPIO3 for VCC:** the DFRobot capacitive sensor draws ~5 mA whenever powered. Wired to the 3V3 rail it would draw continuously through deep sleep (~120 mAh/day). Driving it from a GPIO lets the firmware power the sensor only during the ~150 ms warm-up + sample window each wake. `gpio_hold_en()` plus `gpio_deep_sleep_hold_en()` lock the pin LOW through sleep.

**Note:** To re-pin, change `SOIL_ADC_CHAN` and `SOIL_PWR_GPIO` in [src/soil_moisture.c](src/soil_moisture.c), and update the matching `gpio_hold_en()` call in `enter_deep_sleep()` in [src/main.c](src/main.c).

### Available ADC Pins on ESP32-C6 (ADC1 only)
- GPIO0 → ADC1_CH0 (used by battery monitor)
- GPIO1 → ADC1_CH1 (free)
- GPIO2 → ADC1_CH2 (used by soil moisture AOUT)
- GPIO3 → ADC1_CH3 (used as soil sensor power switch, not as ADC)
- GPIO4 → ADC1_CH4 (free)
- GPIO5 → ADC1_CH5 (free)
- GPIO6 → ADC1_CH6 (free)

## Sensor Calibration

The soil moisture sensor requires calibration for accurate readings. The sensor outputs different voltages depending on the moisture level:

- **Dry conditions (air)**: ~2.5-3.0V
- **Wet conditions (water)**: ~1.0-1.5V

### Calibration Procedure

1. **Measure Dry Value:**
   - Leave the sensor in air (completely dry)
   - Note the voltage reading from the logs
   - Update `SENSOR_DRY_MV` in [src/soil_moisture.c](src/soil_moisture.c#L17)

2. **Measure Wet Value:**
   - Submerge the sensor in water up to the MAX line (do NOT submerge the electronics)
   - Note the voltage reading from the logs
   - Update `SENSOR_WET_MV` in [src/soil_moisture.c](src/soil_moisture.c#L18)

### Default Calibration Values

```c
#define SENSOR_DRY_MV         2800   // Voltage in air (dry)
#define SENSOR_WET_MV         1200   // Voltage in water (wet)
```

Adjust these values based on your actual sensor readings for best accuracy.

## How It Works

The sensor uses capacitive sensing technology:
- Changes in soil moisture alter the dielectric constant
- This changes the capacitance measured by the sensor
- The sensor outputs an analog voltage proportional to moisture
- Lower voltage = more moisture (wet)
- Higher voltage = less moisture (dry)

The implementation:
1. Reads the analog voltage using ESP32's ADC
2. Applies hardware calibration for accurate voltage measurement
3. Converts voltage to percentage using linear interpolation
4. Publishes the moisture percentage via MQTT

## Features

- **Persistent ADC handles**: Sensor is initialized once and reused
- **Multi-sample averaging**: Takes 10 samples and averages for noise reduction
- **Automatic calibration**: Uses ESP-IDF's ADC calibration scheme
- **Percentage output**: Converts voltage to intuitive 0-100% scale
- **MQTT telemetry**: Automatically included in telemetry messages

## MQTT Output Format

The telemetry message includes soil moisture along with battery voltage:

```json
{
  "battery": 4.15,
  "soil_moisture": 67.5,
  "device": "sensor02"
}
```

Published to topic: `zigbee2mqtt/{device_id}` every 30 seconds.

**Fields:**
- `battery`: Battery voltage in volts
- `soil_moisture`: Moisture percentage (0-100%, where 0=dry air, 100=submerged in water)
- `device`: Device identifier configured during WiFi provisioning

## Testing

1. **Build and flash** the firmware to your ESP32-C6
2. **Monitor serial output** to see sensor readings:
   ```
   I (12345) SOIL_MOISTURE: Moisture: 67.5% (1.850 V, 1850 mV)
   ```
3. **Test the sensor:**
   - Observe ~0% when in air
   - Observe ~100% when in water
   - Observe intermediate values in moist soil

## Troubleshooting

### Sensor reads 0% constantly
- Check wiring connections
- Verify the correct GPIO pin is configured
- Ensure sensor has power (3.3V)

### Readings are inaccurate
- Perform calibration procedure
- Check sensor is fully inserted in soil
- Ensure sensor probe is clean

### Sensor not responding
- Check ADC channel configuration
- Verify no conflicts with other ADC channels
- Test with a multimeter to verify sensor output voltage

## Maintenance

- Keep the sensor probe clean
- Avoid submerging the electronics (only the probe)
- Periodically check calibration values
- Replace sensor if readings become erratic
