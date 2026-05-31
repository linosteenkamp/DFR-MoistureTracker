#ifndef OTA_CLIENT_H
#define OTA_CLIENT_H

#ifdef USE_ZIGBEE
#include <stdbool.h>
#include "esp_err.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_command.h"  /* esp_zb_zcl_ota_upgrade_value_message_t */

/* OTA value callback — invoked from the core action handler on
 * ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID. Writes received blocks to the inactive
 * slot and reboots into the new image on FINISH. */
esp_err_t ota_client_on_value(const esp_zb_zcl_ota_upgrade_value_message_t *msg);

/* Add the OTA Upgrade client cluster to an existing cluster list (call while
 * building the endpoint in zigbee_reporter's esp_zb_task). */
void ota_client_add_cluster(esp_zb_cluster_list_t *clusters);

/* Called once after esp_zb_start: registers the OTA action handler + records the
 * client endpoint. Does NOT start querying — that needs server discovery. */
void ota_client_start(uint8_t endpoint);

/* Call from the Zigbee signal handler on join success (stack context). Discovers
 * the coordinator's OTA server and arms periodic image queries. Without this the
 * client never queries and z2m can't deliver an update to it. */
void ota_client_on_joined(void);

/* True if the running image is in pending-verify (freshly OTA'd, not yet confirmed). */
bool ota_client_image_pending_verify(void);

/* Confirm a pending-verify image so the bootloader keeps it (no-op otherwise). */
void ota_client_mark_valid(void);

#endif /* USE_ZIGBEE */
#endif /* OTA_CLIENT_H */
