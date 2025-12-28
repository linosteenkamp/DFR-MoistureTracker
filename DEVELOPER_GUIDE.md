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
- [ ] MQTT publishes every 30 seconds
- [ ] Device reconnects after WiFi loss
- [ ] Device handles MQTT broker disconnect

### Serial Monitor Validation

Look for successful initialization sequence:
```
I MAIN: === FireBeetle ESP32-C6 Application Starting ===
I ADC_MGR: ADC manager initialized
I BATTERY: Battery monitor initialized on ADC1 Channel 0
I SOIL_MOISTURE: Soil moisture sensor initialized on ADC1 Channel 1
I MAIN: WiFi connected successfully
I MQTT_PUB: MQTT connected
I MAIN: === Application Ready ===
```

## Performance Considerations

### Power Consumption

Current implementation (always-on):
- WiFi active: ~80mA
- MQTT keepalive: ~5mA average
- ADC readings: ~2mA for 100ms
- **Total**: ~85mA average

With deep sleep (5 min intervals):
- Active time: ~5 seconds = ~85mA × 5s = 0.118mAh
- Sleep current: ~10µA × 295s = 0.0008mAh
- **Average**: ~0.14mAh per 5 minutes = ~1.7mA average
- **Battery life**: 2000mAh / 1.7mA ≈ 49 days

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
