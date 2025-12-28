# Flash Partition Layout

## Overview

The `partitions.csv` file defines how the ESP32-C6's flash memory is divided into different sections for firmware, data storage, and system configuration.

## Partition Table

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        0x280000,
storage,  data, nvs,     ,        0x4000,
```

## Partition Breakdown

### 1. NVS (Non-Volatile Storage)
- **Name**: `nvs`
- **Type**: `data`
- **SubType**: `nvs`
- **Size**: 24 KB (0x6000 bytes)
- **Purpose**: Main non-volatile storage for WiFi credentials, device ID, and system settings
- **Contents**:
  - WiFi SSID
  - WiFi password
  - Device ID (MQTT topic)
  - System configuration flags
  - Provisioning state

### 2. PHY Init
- **Name**: `phy_init`
- **Type**: `data`
- **SubType**: `phy`
- **Size**: 4 KB (0x1000 bytes)
- **Purpose**: PHY (Physical Layer) initialization data for WiFi/Bluetooth radio calibration
- **Contents**:
  - RF calibration data
  - WiFi PHY parameters
  - Country code settings

### 3. Factory App
- **Name**: `factory`
- **Type**: `app`
- **SubType**: `factory`
- **Size**: 2.5 MB (0x280000 bytes = 2,621,440 bytes)
- **Purpose**: Main application firmware storage
- **Contents**:
  - Compiled application code
  - ESP-IDF framework
  - All libraries and dependencies
  - Text, data, and rodata sections

### 4. Storage
- **Name**: `storage`
- **Type**: `data`
- **SubType**: `nvs`
- **Size**: 16 KB (0x4000 bytes)
- **Purpose**: Additional NVS partition for user data or future expansion
- **Contents**:
  - Currently unused
  - Available for custom data storage
  - Could store calibration values, logs, or sensor history

## Total Flash Usage

| Partition | Size | Percentage |
|-----------|------|------------|
| NVS       | 24 KB | 0.6% |
| PHY Init  | 4 KB | 0.1% |
| Factory   | 2.5 MB | 62.5% |
| Storage   | 16 KB | 0.4% |
| **Total** | **2.54 MB** | **63.6%** |

**Note**: ESP32-C6 typically has 4MB or 8MB flash. Remaining space is reserved for bootloader, partition table, and future OTA updates.

## Flash Memory Layout

```
┌──────────────────────┐ 0x0000
│   Bootloader         │ ~32 KB
├──────────────────────┤ 0x8000
│   Partition Table    │ ~4 KB
├──────────────────────┤ 0x9000
│   NVS (24 KB)        │ WiFi credentials, device ID
├──────────────────────┤ 0xF000
│   PHY Init (4 KB)    │ RF calibration data
├──────────────────────┤ 0x10000
│   Factory App        │ Main firmware (2.5 MB)
│   (2.5 MB)           │
├──────────────────────┤ 0x290000
│   Storage (16 KB)    │ User data / expansion
├──────────────────────┤ 0x294000
│   Free Space         │ Available for OTA, etc.
└──────────────────────┘ End of flash
```

## Key Features

### No OTA Partitions
- Current configuration uses single "factory" app partition
- No OTA (Over-The-Air) update support
- Firmware updates require physical connection and re-flash
- Simplifies flash layout for this application

### Multiple NVS Partitions
- **Primary NVS**: System-critical data (WiFi credentials)
- **Storage NVS**: Additional user data storage
- Separation prevents corruption of critical data

### Large App Partition
- 2.5 MB provides ample space for:
  - ESP-IDF framework (~1.5 MB)
  - Application code (~200 KB)
  - Libraries and assets (~800 KB)
  - Debug symbols (if enabled)

## Modifying Partitions

### When to Modify

**Add OTA Support:**
```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x6000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        0x180000,
ota_0,    app,  ota_0,   ,        0x180000,
ota_1,    app,  ota_1,   ,        0x180000,
otadata,  data, ota,     ,        0x2000,
storage,  data, nvs,     ,        0x4000,
```

**Increase NVS for More Data:**
```csv
nvs,      data, nvs,     ,        0x10000,  # 64 KB instead of 24 KB
```

**Add SPIFFS for File Storage:**
```csv
spiffs,   data, spiffs,  ,        0x100000,  # 1 MB for files
```

### Configuration File

The partition table is referenced in [platformio.ini](platformio.ini):

```ini
[env]
board_build.partitions = partitions.csv
```

### Applying Changes

1. Edit `partitions.csv`
2. Clean build: `platformio run --target clean`
3. Rebuild: `platformio run`
4. Upload: `platformio run --target upload`

**Warning**: Changing partitions erases all data. Backup WiFi credentials first!

## Troubleshooting

### App Too Large Error
```
Error: app partition is too small
```
**Solution**: Increase factory partition size (reduce storage or add compression)

### NVS Initialization Failed
```
E (123) nvs: nvs_flash_init failed
```
**Solutions**:
1. Flash erase: `platformio run --target erase`
2. Check partition alignment (must be 4KB aligned)
3. Verify NVS size is sufficient (minimum 12KB recommended)

### Partition Overlap
```
Error: Partitions overlap
```
**Solution**: Check offsets and sizes don't exceed flash capacity or overlap each other

### Factory Reset Not Working
- Verify NVS partition is large enough (24KB is sufficient)
- Check factory reset code targets correct NVS namespace
- Use `nvs_flash_erase()` if partition corrupted

## Advanced: Partition Flags

Currently unused, but available flags:

- **encrypted**: Partition is encrypted
- **readonly**: Partition cannot be modified at runtime

Example:
```csv
nvs, data, nvs, , 0x6000, encrypted
```

## NVS Namespaces Used

The application uses the following NVS namespaces:

| Namespace | Partition | Purpose |
|-----------|-----------|---------|
| `wifi_config` | nvs | WiFi SSID, password, device ID |
| (none) | storage | Available for future use |

## References

- [ESP-IDF Partition Tables](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/partition-tables.html)
- [NVS Flash Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/storage/nvs_flash.html)
- [OTA Updates Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/system/ota.html)

## Best Practices

1. **Always backup data** before changing partitions
2. **Keep NVS size adequate** - 24KB is good for this application
3. **Align partitions** to 4KB boundaries
4. **Test after changes** - verify all features work
5. **Document custom partitions** if you add more
6. **Consider OTA** if remote updates needed
7. **Monitor flash usage** with `pio run --target size`
