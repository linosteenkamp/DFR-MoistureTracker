#ifdef USE_ZIGBEE
#include "ota_client.h"
#include "ota_ids.h"
#include "fw_version.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"
#include "zcl/esp_zigbee_zcl_command.h"  /* esp_zb_zcl_ota_upgrade_value_message_t */
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "zigbee_reporter.h"

static const char *TAG = "OTA_CLI";
#define OTA_QUERY_INTERVAL_MIN  30   /* minutes between image-version queries */

/* ---- OTA download state (single in-flight upgrade) ---- */
static const esp_partition_t *s_ota_part = NULL;
static esp_ota_handle_t       s_ota_handle = 0;
static bool                   s_ota_in_progress = false;

/* ---- Burst mode + stall watchdog ----
 * During a download we keep the radio up and pause periodic reports; a one-shot
 * timer aborts the transfer if no block arrives within OTA_STALL_TIMEOUT_MS so a
 * dead server can't leave the device stuck awake (battery drain). */
#define OTA_STALL_TIMEOUT_MS  60000
static TimerHandle_t s_stall_timer = NULL;

/* Forward declarations — defined below, but referenced by ota_stall_cb and
 * ota_client_on_value (which appear before / between the definitions). */
static void ota_client_burst_begin(void);
static void ota_client_burst_kick(void);
static void ota_client_burst_end(void);

static void ota_stall_cb(TimerHandle_t t) {
    (void)t;
    ESP_LOGW(TAG, "OTA stalled — aborting, returning to sleepy mode");
    if (s_ota_in_progress) { esp_ota_abort(s_ota_handle); s_ota_in_progress = false; }
    ota_client_burst_end();
}

static void ota_client_burst_begin(void) {
    zigbee_reporter_set_reports_paused(true);   /* no ADC/e-paper churn mid-OTA */
    esp_zb_sleep_enable(false);                 /* no light sleep during download */
    esp_zb_set_rx_on_when_idle(true);           /* keep receiver up for blocks */
    if (!s_stall_timer) {
        s_stall_timer = xTimerCreate("ota_stall", pdMS_TO_TICKS(OTA_STALL_TIMEOUT_MS),
                                     pdFALSE, NULL, ota_stall_cb);
    }
    if (s_stall_timer) xTimerStart(s_stall_timer, 0);
    ESP_LOGI(TAG, "burst mode ON");
}

static void ota_client_burst_kick(void) {
    if (s_stall_timer) xTimerReset(s_stall_timer, 0);
}

static void ota_client_burst_end(void) {
    if (s_stall_timer) xTimerStop(s_stall_timer, 0);
    esp_zb_set_rx_on_when_idle(false);          /* back to sleepy ED */
    esp_zb_sleep_enable(true);
    zigbee_reporter_set_reports_paused(false);
    ESP_LOGI(TAG, "burst mode OFF");
}

esp_err_t ota_client_on_value(const esp_zb_zcl_ota_upgrade_value_message_t *msg)
{
    switch (msg->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        s_ota_part = esp_ota_get_next_update_partition(NULL);
        ESP_LOGI(TAG, "OTA start -> slot %s", s_ota_part ? s_ota_part->label : "?");
        if (!s_ota_part ||
            esp_ota_begin(s_ota_part, OTA_SIZE_UNKNOWN, &s_ota_handle) != ESP_OK) {
            return ESP_FAIL;
        }
        s_ota_in_progress = true;
        ota_client_burst_begin();
        return ESP_OK;

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        if (!s_ota_in_progress) return ESP_FAIL;
        ota_client_burst_kick();
        return esp_ota_write(s_ota_handle, msg->payload, msg->payload_size);

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        return ESP_OK;   /* allow the upgrade to proceed */

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        if (esp_ota_end(s_ota_handle) != ESP_OK ||
            esp_ota_set_boot_partition(s_ota_part) != ESP_OK) {
            ESP_LOGE(TAG, "OTA finalize failed");
            s_ota_in_progress = false;
            ota_client_burst_end();
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "OTA complete — rebooting into new image");
        esp_restart();
        return ESP_OK;   /* not reached */

    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
        if (s_ota_in_progress) {
            esp_ota_abort(s_ota_handle);
            s_ota_in_progress = false;
            ota_client_burst_end();
        }
        ESP_LOGW(TAG, "OTA aborted (status %d)", msg->upgrade_status);
        return ESP_OK;

    default:
        return ESP_OK;
    }
}

/* Single global core action handler — dispatches on callback id.
 * No other module registers one (verified by grep); if that changes, the OTA
 * dispatch must move into the shared handler rather than registering a second. */
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id, const void *message)
{
    if (cb_id == ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID) {
        return ota_client_on_value((const esp_zb_zcl_ota_upgrade_value_message_t *)message);
    }
    return ESP_OK;
}

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
    esp_zb_core_action_handler_register(zb_action_handler);
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
