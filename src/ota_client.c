#ifdef USE_ZIGBEE
#include "ota_client.h"
#include "ota_ids.h"
#include "fw_version.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "OTA_CLI";
#define OTA_QUERY_INTERVAL_MIN  30   /* minutes between image-version queries */

void ota_client_add_cluster(esp_zb_cluster_list_t *clusters)
{
    esp_zb_ota_cluster_cfg_t ota_cfg = {
        .ota_upgrade_file_version        = FW_VERSION_U32,
        .ota_upgrade_manufacturer        = OTA_MANUFACTURER_CODE,
        .ota_upgrade_image_type          = OTA_IMAGE_TYPE,
        .ota_upgrade_downloaded_file_ver = FW_VERSION_U32,
    };
    esp_zb_attribute_list_t *ota_attrs = esp_zb_ota_cluster_create(&ota_cfg);
    esp_zb_cluster_list_add_ota_cluster(clusters, ota_attrs, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    ESP_LOGI(TAG, "OTA client cluster added (fw %s = 0x%08x)",
             FW_VERSION_STR, (unsigned)FW_VERSION_U32);
}

void ota_client_start(uint8_t endpoint)
{
    esp_zb_ota_upgrade_client_query_interval_set(endpoint, OTA_QUERY_INTERVAL_MIN);

    /* DECISION: we do NOT call esp_zb_ota_upgrade_client_query_image_req() here.
     * Per docs/ota-api-notes.md (sections 4 & 9) its signature is
     * (uint16_t server_ep, uint8_t server_addr) and it requires the OTA server's
     * endpoint and short address to already be known. At start time the device
     * has not yet discovered/bound an OTA server, so any args would be guesses.
     * Instead we rely on the periodic interval query above, which the stack
     * drives automatically once the OTA server is known. The manual req can be
     * issued from the value callback in a later task if directed discovery is
     * needed. */
    ESP_LOGI(TAG, "OTA client started (query every %d min)", OTA_QUERY_INTERVAL_MIN);
}

bool ota_client_image_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        return st == ESP_OTA_IMG_PENDING_VERIFY;
    }
    return false;
}

void ota_client_mark_valid(void)
{
    if (ota_client_image_pending_verify()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "New image confirmed valid (rollback cancelled)");
    }
}
#endif /* USE_ZIGBEE */
