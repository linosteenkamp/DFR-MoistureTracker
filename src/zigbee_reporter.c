/**
 * @file zigbee_reporter.c
 * @brief Zigbee end-device stack init + BDB join using the classic esp_zb_* API.
 *        CONFIG_ZB_SDK_1xx=y enables the compat shim in esp-zigbee-lib 2.x.
 *        Sensor reporting is wired in a later task (Task 4).
 *
 * The entire body is wrapped in #ifdef USE_ZIGBEE so that the WiFi build
 * (no -DUSE_ZIGBEE) compiles this file as empty — no undefined references.
 */

#ifdef USE_ZIGBEE

#include "zigbee_reporter.h"

/* Classic esp_zb_* API, native to esp-zigbee-lib 1.6.x (headers at the
 * include root; no compat/ prefix). Matched pair with esp-zboss-lib 1.6.x. */
#include "esp_zigbee_core.h"         /* esp_zb_cfg_t, esp_zb_init, ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK, ... */
#include "platform/esp_zigbee_platform.h"  /* esp_zb_platform_config_t, esp_zb_platform_config() */
#include "nwk/esp_zigbee_nwk.h"     /* ESP_ZB_DEVICE_TYPE_ED, ESP_ZB_ED_AGING_TIMEOUT_64MIN */
#include "bdb/esp_zigbee_bdb_commissioning.h" /* esp_zb_bdb_start_top_level_commissioning, esp_zb_bdb_is_factory_new */
#include "zdo/esp_zigbee_zdo_common.h"        /* esp_zb_app_signal_t, signal type enum, esp_zb_zdo_signal_to_string */
#include "esp_zigbee_cluster.h"      /* esp_zb_basic_cluster_create, esp_zb_identify_cluster_create,
                                               esp_zb_zcl_cluster_list_create, esp_zb_cluster_list_add_*,
                                               esp_zb_basic_cluster_cfg_t, esp_zb_identify_cluster_cfg_t */
#include "esp_zigbee_attribute.h"    /* esp_zb_basic_cluster_add_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE */
#include "esp_zigbee_endpoint.h"     /* esp_zb_ep_list_create, esp_zb_ep_list_add_ep */
#include "zcl/esp_zigbee_zcl_common.h"   /* ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
                                                    esp_zb_endpoint_config_t */
#include "zcl/esp_zigbee_zcl_basic.h"    /* ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID/MODEL_IDENTIFIER_ID */
#include "zcl/esp_zigbee_zcl_core.h"     /* esp_zb_device_register */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ZB_RPT";

/* Joined flag — set by the signal handler, polled by zigbee_reporter_wait_ready(). */
static volatile bool s_joined = false;

/* Thin wrapper so we can pass network-steering as an esp_zb_scheduler_alarm callback.
 * esp_zb_bdb_start_top_level_commissioning is a #define (not a real function), so
 * it cannot be used as a function pointer directly. */
static void steering_alarm_cb(uint8_t mode)
{
    esp_zb_bdb_start_top_level_commissioning((esp_zb_bdb_commissioning_mode_mask_t)mode);
}

#define APP_ENDPOINT  1U

/* ZCL strings are length-prefixed: first byte = character count, then ASCII data. */
#define MANUF_NAME  "\x0B" "DFRobot-DIY"    /* 11 chars */
#define MODEL_ID    "\x0E" "DFR-SoilSensor" /* 14 chars */

/* ============================================================
 * Required application signal callback (called by the stack).
 * ============================================================ */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t           *p_sg_p      = signal_struct->p_app_signal;
    esp_err_t           err_status  = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        /* Stack is ready — kick off BDB initialisation. */
        ESP_LOGI(TAG, "Stack ready, starting BDB init");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory-new device — starting network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                /* Already has network credentials; stack rejoined automatically. */
                ESP_LOGI(TAG, "Rebooted onto existing network, short addr 0x%04hx",
                         esp_zb_get_short_address());
                s_joined = true;
            }
        } else {
            ESP_LOGW(TAG, "%s: status %d",
                     (sig_type == ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START) ?
                     "FIRST_START" : "REBOOT", err_status);
        }
        break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ext_pan;
            esp_zb_get_extended_pan_id(ext_pan);
            ESP_LOGI(TAG, "BDB steering complete — joined network, short addr 0x%04hx",
                     esp_zb_get_short_address());
            s_joined = true;
        } else {
            ESP_LOGW(TAG, "BDB steering failed (status %d), retrying in 1 s", err_status);
            /* Schedule a retry.  We use the wrapper because esp_zb_bdb_start_top_level_commissioning
             * is a macro (wrapping TO_ESP_ERR(…)) and therefore cannot be taken as a pointer. */
            esp_zb_scheduler_alarm(steering_alarm_cb,
                                   (uint8_t)ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                   1000U);
        }
        break;

    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %d",
                 esp_zb_zdo_signal_to_string(sig_type), (unsigned)sig_type, err_status);
        break;
    }
}

/* ============================================================
 * Zigbee stack task — runs forever inside esp_zb_stack_main_loop()
 * ============================================================ */

static void esp_zb_task(void *pv)
{
    (void)pv;

    /* ---- Stack configuration ---- */
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout  = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive  = 3000U,   /* ms between keep-alive polls to parent */
        },
    };
    esp_zb_init(&zb_cfg);

    /* ---- Minimal endpoint: Basic + Identify (enough to commission) ---- */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = 8,
        .power_source = 3,  /* 0x03 = battery */
    };
    esp_zb_attribute_list_t *basic_attrs = esp_zb_basic_cluster_create(&basic_cfg);

    /* Add optional string attributes: ManufacturerName and ModelIdentifier. */
    /* The cast discards const — the API takes void*, but does not mutate the string. */
    esp_zb_basic_cluster_add_attr(basic_attrs,
                                  ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                  (void *)MANUF_NAME);
    esp_zb_basic_cluster_add_attr(basic_attrs,
                                  ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                  (void *)MODEL_ID);

    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *identify_attrs = esp_zb_identify_cluster_create(&identify_cfg);

    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(clusters, basic_attrs,
                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(clusters, identify_attrs,
                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint            = APP_ENDPOINT,
        .app_profile_id      = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id       = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version  = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
    esp_zb_device_register(ep_list);

    /* Scan all 2.4 GHz channels (11–26). */
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    /* Start the stack (autostart=false — we drive commissioning via signal handler). */
    ESP_ERROR_CHECK(esp_zb_start(false));

    /* Blocks until stack is deinitialised (never returns under normal operation). */
    esp_zb_stack_main_loop();

    vTaskDelete(NULL);
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t zigbee_reporter_init(void)
{
    ESP_LOGI(TAG, "Initialising Zigbee end-device (esp_zb_* compat API)");

    /* Configure the native 802.15.4 radio (no host/RCP connection). */
    esp_zb_platform_config_t pcfg = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&pcfg));

    /* Create the stack task.  Stack size 4096 and priority 5 match HA examples. */
    BaseType_t ret = xTaskCreate(esp_zb_task, "esp_zb_task", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Zigbee task created — network join is asynchronous");
    return ESP_OK;
}

bool zigbee_reporter_wait_ready(uint32_t timeout_ms)
{
    uint32_t waited = 0U;
    const uint32_t poll_ms = 100U;

    while (!s_joined && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited += poll_ms;
    }

    return s_joined;
}

esp_err_t zigbee_reporter_report(float soil_pct, float battery_v, float battery_pct)
{
    /* Stub — real ZCL attribute reporting wired in Task 4. */
    ESP_LOGI(TAG, "report (stub): soil=%.1f%% batt=%.2fV (%.0f%%)",
             soil_pct, battery_v, battery_pct);
    return ESP_OK;
}

#endif /* USE_ZIGBEE */
