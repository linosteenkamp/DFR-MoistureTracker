#ifndef OTA_IDS_H
#define OTA_IDS_H

#include <stdint.h>

/* Fixed product identity for Zigbee OTA. These three values MUST match across
 * the firmware, the .ota image header, and the z2m OTA index. */
#define OTA_MANUFACTURER_CODE  0xFEFEu   /* chosen DIY 16-bit code */
#define OTA_IMAGE_TYPE         0x0001u
#define OTA_MODEL_ID           "DFR-SoilSensor"

/* Pack semver into the 32-bit Zigbee fileVersion so it displays correctly in z2m.
 * z2m renders the version as the 8-char hex string split "AB.CC-DE.FF" (via
 * fileVersion2String: hex[0].hex[1].hex[2:4]-hex[4].hex[5].hex[6:8]). To make the
 * MAJOR appear as the leading digit, the semver must live in the HIGH hex digits:
 *
 *   nibble7=major  nibble6=minor  byte2=patch  bytes1..0=0x0000
 *   e.g. v1.0.2 => 0x10020000 => z2m shows "1.0.02-0.0.00"
 *
 * Monotonic: higher semver => larger uint32 (major/minor each 0..15, patch 0..255).
 * Every legacy scheme used top byte 0x01 (<= 0x01ffffff), so any major>=1 here is
 * strictly larger and pre-fix firmware is still offered the upgrade.
 * `build` is unused (kept in the signature for call-site compatibility). */
#define OTA_PACK_VERSION(major, minor, patch, build)            \
    (((uint32_t)((major) & 0xFu) << 28) |                       \
     ((uint32_t)((minor) & 0xFu) << 24) |                       \
     ((uint32_t)((patch) & 0xFFu) << 16))

#endif /* OTA_IDS_H */
