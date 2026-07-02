#ifndef OTA_IDS_H
#define OTA_IDS_H

#include <stdint.h>

/* Fixed product identity for Zigbee OTA. These three values MUST match across
 * the firmware, the .ota image header, and the z2m OTA index. */
#define OTA_MANUFACTURER_CODE  0xFEFEu   /* chosen DIY 16-bit code */
#define OTA_IMAGE_TYPE         0x0001u
#define OTA_MODEL_ID           "DFR-SoilSensor"

/* Pack semver into the 32-bit Zigbee fileVersion as 0xMMmmpp bb
 * (major, minor, patch, build). Monotonic: higher semver => larger uint32. */
#define OTA_PACK_VERSION(major, minor, patch, build)            \
    (((uint32_t)(major) << 24) | ((uint32_t)(minor) << 16) |    \
     ((uint32_t)(patch) << 8)  |  (uint32_t)(build))

#endif /* OTA_IDS_H */
