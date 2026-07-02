#ifndef OTA_IDS_H
#define OTA_IDS_H

#include <stdint.h>

/* Fixed product identity for Zigbee OTA. These three values MUST match across
 * the firmware, the .ota image header, and the z2m OTA index. */
#define OTA_MANUFACTURER_CODE  0xFEFEu   /* chosen DIY 16-bit code */
#define OTA_IMAGE_TYPE         0x0001u
#define OTA_MODEL_ID           "DFR-SoilSensor"

/* Pack semver into the 32-bit Zigbee fileVersion. z2m displays the LOW 3 bytes as
 * major.minor.patch (it drops the top byte), so the semver lives there. The fixed
 * 0x01 top byte keeps values above the legacy 0x010000xx scheme so upgrades from
 * pre-v1.0.2 firmware are still offered. Monotonic: higher semver => larger uint32.
 * `build` is unused (kept in the signature for call-site compatibility). */
#define OTA_PACK_VERSION(major, minor, patch, build)            \
    (((uint32_t)0x01u << 24) | ((uint32_t)(major) << 16) |      \
     ((uint32_t)(minor) << 8)  |  (uint32_t)(patch))

#endif /* OTA_IDS_H */
