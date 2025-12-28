# Factory Reset Guide

## Overview

The factory reset feature allows users to clear all stored configuration (WiFi credentials and device ID) by pressing and holding a physical button for 5 seconds.

## Hardware Setup

### Required Components
- 1× Momentary push button (tactile switch)
- 2× Jumper wires

### Wiring
```
FireBeetle 2 ESP32-C6
┌─────────────────┐
│                 │
│  GPIO 20  ──────┼──┐
│                 │  │
│  GND      ──────┼──┤
│                 │  │
└─────────────────┘  │
                     │
                  [Button]
                     │
                  (Press)
```

**Connection:**
- Button Pin 1 → GPIO 20
- Button Pin 2 → GND

**Note:** Internal pull-up resistor is enabled, so no external resistor needed.

## How It Works

### Button State Detection
- **Normal (unpressed)**: GPIO 20 is HIGH (3.3V) via internal pull-up
- **Pressed**: GPIO 20 is LOW (0V) - button connects to GND
- **Debouncing**: 50ms delay prevents false triggers

### Reset Sequence
1. User presses and holds button
2. Device detects button press (GPIO 20 goes LOW)
3. Countdown begins: 5 seconds to factory reset
4. Every second, progress is logged to serial monitor
5. After 5 seconds of continuous press:
   - WiFi SSID cleared from NVS
   - WiFi password cleared from NVS
   - Device ID cleared from NVS
   - Device restarts automatically
6. Device boots into provisioning mode

### Cancellation
- Releasing button before 5 seconds cancels the reset
- No changes are made if button is released early
- Countdown resets on next button press

## Serial Monitor Output

### Successful Factory Reset
```
I (12000) FACTORY_RESET: Button pressed - hold for 5 seconds to factory reset
I (13000) FACTORY_RESET: Hold for 4 more seconds...
I (14000) FACTORY_RESET: Hold for 3 more seconds...
I (15000) FACTORY_RESET: Hold for 2 more seconds...
I (16000) FACTORY_RESET: Hold for 1 more seconds...
W (17000) FACTORY_RESET: Factory reset triggered!
I (17005) WIFI_CRED: Clearing WiFi SSID from NVS
I (17010) WIFI_CRED: Clearing WiFi password from NVS
I (17015) WIFI_CRED: Clearing device ID from NVS
W (17020) FACTORY_RESET: Restarting in 2 seconds...
```

### Cancelled Reset (Button Released Early)
```
I (12000) FACTORY_RESET: Button pressed - hold for 5 seconds to factory reset
I (13000) FACTORY_RESET: Hold for 4 more seconds...
I (14000) FACTORY_RESET: Hold for 3 more seconds...
I (14500) FACTORY_RESET: Button released - reset cancelled
```

## Configuration

### GPIO Pin
Default: GPIO 20

To change, edit [include/factory_reset.h](include/factory_reset.h):
```c
#define FACTORY_RESET_GPIO    GPIO_NUM_20
```

### Hold Duration
Default: 5 seconds

To change, edit [src/factory_reset.c](src/factory_reset.c):
```c
#define HOLD_TIME_MS          5000  // Change to desired milliseconds
```

### Check Interval
Default: 1 second (checked every telemetry cycle)

Factory reset is checked in the main telemetry loop. To change frequency, edit [src/main.c](src/main.c):
```c
#define TELEMETRY_INTERVAL_MS (30 * 1000)  // Also affects reset check rate
```

## Implementation Details

### Initialization
```c
esp_err_t factory_reset_init(void);
```
- Configures GPIO 20 as input with pull-up
- Called once during system initialization
- Must be called before `factory_reset_check()`

### Checking for Reset
```c
void factory_reset_check(void);
```
- Checks button state and manages countdown
- Called repeatedly in main telemetry loop
- Non-blocking - returns immediately if button not pressed
- Handles timing and NVS clearing

### State Machine
1. **IDLE**: Waiting for button press
2. **COUNTING**: Button held, counting down
3. **TRIGGERED**: 5 seconds elapsed, clearing data
4. **CANCELLED**: Button released early

## After Factory Reset

### Device Behavior
1. Device restarts automatically
2. Boots up with no WiFi credentials
3. Enters provisioning mode
4. Creates WiFi AP: `FireBeetle_C6_Prov`
5. Waits for user to configure WiFi again

### Reprovisioning Steps
1. Connect to `FireBeetle_C6_Prov` WiFi network
2. Navigate to `http://192.168.4.1`
3. Enter WiFi credentials and device ID
4. Submit and device will reconnect

See [WIFI_PROVISIONING.md](WIFI_PROVISIONING.md) for detailed provisioning instructions.

## Use Cases

### When to Use Factory Reset

**Good Reasons:**
- Changing WiFi network (new router, moved location)
- Changing device ID for MQTT topic organization
- Selling/transferring device to another user
- WiFi password changed and device can't reconnect
- Testing provisioning functionality

**Alternative Solutions:**
- **WiFi connection fails**: Device auto-resets after 30 seconds
- **MQTT issues**: Usually configuration, not credential problem
- **Software bugs**: Upload new firmware instead

## Troubleshooting

### Button doesn't respond
**Symptom**: Pressing button has no effect

**Solutions:**
1. Check wiring - ensure button connects GPIO 20 to GND
2. Check button itself - test with multimeter for continuity
3. Verify GPIO 20 not used elsewhere in code
4. Check serial monitor - should see "Button pressed" message
5. Try different GPIO pin if GPIO 20 is damaged

### Button triggers immediately
**Symptom**: Reset happens without holding button

**Solutions:**
1. Check for short circuit on GPIO 20
2. Verify pull-up resistor is enabled
3. Add external 10kΩ pull-up resistor if internal resistor faulty
4. Increase `HOLD_TIME_MS` for longer hold requirement

### Reset doesn't clear credentials
**Symptom**: Device still has WiFi credentials after reset

**Solutions:**
1. Check serial monitor for error messages during NVS clear
2. Verify NVS partition is not corrupted
3. Try erasing flash: `platformio run --target erase`
4. Reflash firmware completely

### Button too sensitive (bouncing)
**Symptom**: Multiple triggers from single press

**Solutions:**
1. Code already has 50ms debounce delay
2. Add hardware debouncing (0.1µF capacitor across button)
3. Increase `DEBOUNCE_TIME_MS` in code

## Safety Features

### Debouncing
- 50ms delay prevents electrical noise from false triggers
- Ensures clean button press detection

### Countdown Display
- User gets clear feedback via serial monitor
- Can cancel if pressed accidentally
- Progress indicators prevent surprises

### Confirmation Required
- Must hold for full 5 seconds
- Prevents accidental resets from brief touches
- User has time to release if unintentional

### Automatic Restart
- Device automatically reboots after reset
- Ensures clean state
- Enters provisioning mode immediately

## Advanced Usage

### Programmatic Factory Reset
You can trigger factory reset from code:

```c
#include "wifi_credentials.h"

// Clear all credentials
wifi_credentials_clear();

// Restart device
esp_restart();
```

### Custom Reset Actions
Extend factory reset to clear additional data:

Edit [src/factory_reset.c](src/factory_reset.c):
```c
static void perform_factory_reset(void) {
    ESP_LOGW(TAG, "Factory reset triggered!");
    
    // Clear WiFi credentials (existing)
    wifi_credentials_clear();
    
    // Add your custom clearing here
    // Example: Clear user settings
    nvs_handle_t handle;
    nvs_open("user_settings", NVS_READWRITE, &handle);
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    
    // Restart
    ESP_LOGW(TAG, "Restarting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
```

## Technical Specifications

- **GPIO Pin**: GPIO 20 (configurable)
- **Input Mode**: Pull-up input
- **Trigger Level**: LOW (0V)
- **Hold Time**: 5000ms (5 seconds)
- **Debounce Time**: 50ms
- **Check Interval**: 1000ms (every telemetry cycle)
- **Power Consumption**: <1µA (pull-up resistor)

## References

- [ESP-IDF GPIO Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/peripherals/gpio.html)
- [ESP-IDF NVS Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/storage/nvs_flash.html)
- [DFRobot FireBeetle 2 ESP32-C6 Pinout](https://wiki.dfrobot.com/SKU_DFR0868_FireBeetle_2_Board_ESP32_C6)
