# Battery Monitoring Guide

## Overview

The FireBeetle 2 ESP32-C6 includes built-in battery voltage monitoring using a hardware voltage divider and ADC (Analog-to-Digital Converter).

## Hardware Configuration

### Battery Connection
- The FireBeetle 2 ESP32-C6 has a built-in JST connector for LiPo batteries
- Voltage range: 3.0V - 4.2V (typical LiPo battery)
- Built-in charging circuit when connected via USB

### Voltage Divider Circuit
- **Location**: GPIO 0 (ADC1 Channel 0)
- **Divider Ratio**: 2:1 (two 1MΩ resistors in series)
- **Input Range**: Measures up to ~6V (half of battery voltage at ADC pin)
- **ADC Input**: 0 - 3.1V (after voltage divider)

### Circuit Diagram
```
Battery+ ────┬──────────── FireBeetle VCC
             │
            1MΩ
             │
             ├──────────── GPIO 0 (ADC1_CH0)
             │
            1MΩ
             │
Battery- ────┴──────────── GND
```

## How It Works

### ADC Configuration
- **Unit**: ADC1 (shared with soil moisture sensor)
- **Channel**: Channel 0 (GPIO 0)
- **Attenuation**: 12dB (allows measurement up to ~3.1V)
- **Bit Width**: 12-bit (values 0-4095)
- **Calibration**: Uses ESP-IDF curve fitting calibration

### Voltage Calculation
1. ADC reads raw value (0-4095)
2. Calibration converts to millivolts at GPIO pin
3. Multiply by 2.0 to account for voltage divider
4. Convert to volts

```c
// Example
raw_adc = 2048              // Mid-range reading
pin_voltage = 1500 mV       // After calibration
battery = 1500 mV × 2 = 3000 mV = 3.0 V
```

### Multi-Sample Averaging
- Takes 10 consecutive readings
- Averages the results
- Reduces noise and improves accuracy

## Implementation

### Initialization
```c
esp_err_t battery_monitor_init(void);
```
- Configures ADC channel for GPIO 0
- Creates calibration handle via ADC manager
- Must be called after `adc_manager_init()`

### Reading Voltage
```c
float battery_monitor_read_voltage(void);
```
- Returns battery voltage in volts (e.g., 3.85)
- Returns 0.0 if not initialized or error occurs
- Automatically applies voltage divider correction

### Cleanup
```c
esp_err_t battery_monitor_deinit(void);
```
- Releases calibration handle reference
- Called automatically on shutdown

## Battery Status Interpretation

### Voltage Ranges
| Voltage | Battery State | Percentage (approx) |
|---------|---------------|---------------------|
| 4.2V    | Fully Charged | 100%                |
| 4.0V    | High          | 85%                 |
| 3.7V    | Nominal       | 50%                 |
| 3.5V    | Low           | 25%                 |
| 3.3V    | Critical      | 10%                 |
| 3.0V    | Empty         | 0%                  |

**Note**: These are approximate values. Actual percentage depends on battery chemistry and discharge curve.

## Integration with MQTT

Battery voltage is automatically published every 30 seconds:

```json
{
  "battery": 3.85,
  "soil_moisture": 65.0,
  "device": "sensor02"
}
```

Topic: `zigbee2mqtt/{device_id}`

## Serial Monitor Output

Example log messages:

```
I (1234) BATTERY: Initializing battery monitor
I (1240) BATTERY: Battery monitor initialized on ADC1 Channel 0
I (5000) BATTERY: Raw ADC: 2450, Pin voltage: 1850 mV, Battery: 3.700 V
```

## Troubleshooting

### Battery reads 0.0V
- Check battery is connected to JST connector
- Verify battery has charge (should be > 3.0V)
- Check ADC manager initialized before battery monitor

### Inaccurate readings
- Voltage readings may fluctuate ±0.05V due to ADC noise
- Multi-sample averaging helps but some variation is normal
- Calibration values are device-specific

### Very high voltage readings (>4.5V)
- Check voltage divider resistors (should be 1MΩ each)
- May indicate hardware issue with FireBeetle board
- Do not exceed 4.2V for LiPo batteries

### ADC initialization fails
- Error: "ADC manager not initialized"
  - Ensure `adc_manager_init()` is called first in `main.c`
- Error: "Failed to configure ADC channel"
  - Check for GPIO conflicts
  - GPIO 0 should not be used for other purposes

## Power Consumption Considerations

### Reading Frequency
- Current implementation: every 30 seconds with MQTT publish
- ADC reading itself is very low power (~1-2mA for ~100ms)
- Consider reducing frequency for battery life optimization

### Deep Sleep Mode (Future Enhancement)
For maximum battery life, consider:
1. Read sensors
2. Publish to MQTT
3. Enter deep sleep for X minutes
4. Wake up and repeat

This can extend battery life from days to months depending on sleep duration.

## Technical Specifications

- **ADC Resolution**: 12-bit (4096 steps)
- **ADC Reference**: Internal, calibrated
- **Voltage Accuracy**: ±50mV typical
- **Sample Rate**: 10 samples averaged
- **Sample Time**: ~100ms total
- **Voltage Range**: 0 - 6.2V (3.1V at ADC after divider)

## Code Example

```c
// Initialize ADC manager first
adc_manager_init();

// Initialize battery monitor
if (battery_monitor_init() == ESP_OK) {
    // Read voltage
    float voltage = battery_monitor_read_voltage();
    printf("Battery: %.2f V\n", voltage);
    
    // Interpret battery level
    if (voltage > 4.0) {
        printf("Battery level: High\n");
    } else if (voltage > 3.5) {
        printf("Battery level: Normal\n");
    } else if (voltage > 3.3) {
        printf("Battery level: Low\n");
    } else {
        printf("Battery level: Critical - Charge Soon!\n");
    }
}

// Cleanup when done
battery_monitor_deinit();
```

## References

- [ESP-IDF ADC Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/adc_oneshot.html)
- [DFRobot FireBeetle 2 ESP32-C6 Wiki](https://wiki.dfrobot.com/SKU_DFR0868_FireBeetle_2_Board_ESP32_C6)
- LiPo Battery Discharge Curves
