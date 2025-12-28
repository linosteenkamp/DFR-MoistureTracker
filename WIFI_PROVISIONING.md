# WiFi Provisioning Guide

## Overview

This project now includes WiFi provisioning support. The device can be configured with WiFi credentials without hardcoding them in the firmware.

## How It Works

1. **First Boot (Not Provisioned)**
   - Device checks NVS for stored WiFi credentials
   - If not found, starts provisioning mode
   - Creates WiFi Access Point: `FireBeetle_C6_Prov` (open network)
   - Waits for user to configure WiFi

2. **Provisioning Process**
   - Connect your phone/computer to `FireBeetle_C6_Prov` WiFi network
   - Open browser and go to: `http://192.168.4.1`
   - Enter your WiFi credentials (SSID and password)
   - Device saves credentials to NVS and restarts

3. **Subsequent Boots (Provisioned)**
   - Device loads WiFi credentials from NVS
   - Connects to your WiFi network
   - Starts normal operation (MQTT, battery monitoring, etc.)

## Provisioning Methods

### Method 1: Web Browser (Current Implementation)
- Connect to `FireBeetle_C6_Prov` AP
- Browse to `http://192.168.4.1`
- Use the web interface to configure

### Method 2: ESP BLE Provisioning App (Future)
- Download "ESP BLE Provisioning" app (iOS/Android)
- Scan for device
- Configure via Bluetooth

## Reset Provisioning

If you need to reconfigure WiFi:

**Option 1: Automatic Reset**
- If device fails to connect after 30 seconds, it automatically erases stored credentials and restarts in provisioning mode

**Option 2: Manual Reset (Code)**
```c
nvs_handle_t nvs_handle;
nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
nvs_erase_all(nvs_handle);
nvs_commit(nvs_handle);
nvs_close(nvs_handle);
esp_restart();
```

**Option 3: Flash Erase**
```bash
platformio run --target erase
platformio run --target upload
```

## Configuration

### Provisioning AP Settings
In `http_request_example_main.c`:
```c
#define PROV_AP_SSID          "FireBeetle_C6_Prov"
#define PROV_AP_PASSWORD      ""  // Empty = open network
```

### NVS Storage Keys
```c
#define NVS_NAMESPACE         "wifi_config"
#define NVS_KEY_SSID          "ssid"
#define NVS_KEY_PASSWORD      "password"
#define NVS_KEY_PROVISIONED   "provisioned"
```

## Testing

1. **First Boot Test**
   ```bash
   platformio run --target upload
   platformio device monitor
   ```
   - Look for: "Device not provisioned - starting provisioning mode"
   - Look for: "Connect to WiFi AP: FireBeetle_C6_Prov"

2. **Connect to Provisioning AP**
   - Use phone/laptop to connect to `FireBeetle_C6_Prov`
   - Open browser → `http://192.168.4.1`

3. **Enter Credentials**
   - Enter your WiFi SSID and password
   - Click "Submit" or "Connect"

4. **Verify Connection**
   - Device should restart
   - Monitor logs: "WiFi connected successfully"
   - Should see MQTT connection

## Troubleshooting

### Device not creating AP
- Check logs for errors during provisioning initialization
- Verify WiFi SoftAP support is enabled in sdkconfig
- Try: `platformio run -t menuconfig` → Component config → ESP32-specific → WiFi

### Can't connect to provisioning AP
- Check if AP name is `FireBeetle_C6_Prov`
- It's an open network (no password)
- Try forgetting and reconnecting
- Check your device supports 2.4GHz WiFi (ESP32-C6 doesn't support 5GHz)

### Credentials saved but not connecting
- Check if your WiFi SSID/password are correct
- Check WiFi signal strength
- Look for connection errors in logs
- Device will auto-reset after 30 failed seconds

### Want to change to BLE provisioning
1. Change in code:
   ```c
   .scheme = wifi_prov_scheme_ble,  // Instead of softap
   ```
2. Update sdkconfig:
   ```
   CONFIG_WIFI_PROV_TRANSPORT_BLE=y
   ```
3. Use ESP BLE Provisioning app

## Security Notes

- Current implementation uses `WIFI_PROV_SECURITY_1` (encrypted provisioning)
- SoftAP is open (no password) - anyone nearby can connect during provisioning
- Consider setting `PROV_AP_PASSWORD` to a PIN for added security
- Credentials are stored in NVS (can be encrypted with NVS encryption feature)

## Next Steps

- [ ] Add BLE provisioning support
- [ ] Add custom provisioning handlers
- [ ] Implement factory reset button (hold GPIO button for 5s)
- [ ] Add QR code provisioning
- [ ] Encrypt NVS storage
