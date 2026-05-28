/**
 * @file zigbee_reporter.c
 * @brief Zigbee end-device stack init + BDB join (Task 3).
 *        Sensor reporting wired in Task 4.
 *
 * The entire body is wrapped in #ifdef USE_ZIGBEE so that the WiFi build
 * (no -DUSE_ZIGBEE) compiles this file as empty — no undefined references.
 */

#ifdef USE_ZIGBEE

#include "zigbee_reporter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

/* Native ezbee API — requires CONFIG_ZB_SDK_1xx=n (default) */
#include "esp_zigbee.h"          /* esp_zigbee_init, esp_zigbee_start, esp_zigbee_launch_mainloop */
#include "ezbee/core.h"          /* ezb_core_init, ezb_set_channel_mask */
#include "ezbee/nwk.h"           /* EZB_NWK_DEVICE_TYPE_END_DEVICE, ezb_nwk_ed_timeout_e */
#include "ezbee/bdb.h"           /* ezb_bdb_start_top_level_commissioning, EZB_BDB_MODE_NETWORK_STEERING */
#include "ezbee/app_signals.h"   /* ezb_app_signal_add_handler, signal types */
#include "ezbee/af.h"            /* ezb_af_create_device_desc, ezb_af_create_endpoint_desc, etc. */
#include "ezbee/zcl.h"           /* cluster includes */
#include "ezbee/zcl/cluster/basic_desc.h"    /* ezb_zcl_basic_create_cluster_desc */
#include "ezbee/zcl/cluster/identify_desc.h" /* ezb_zcl_identify_create_cluster_desc */
#include "ezbee/zcl/zcl_type.h"              /* EZB_ZCL_CLUSTER_SERVER, EZB_ZCL_ATTR_TYPE_STRING, etc. */

static const char *TAG = "ZB_RPT";

/* HA profile ID (0x0104) and a generic HA device ID (0x0000 = non-specific) */
#define ZB_HA_PROFILE_ID         0x0104U
#define ZB_HA_DEVICE_ID_GENERIC  0x0000U
#define ZB_ENDPOINT_ID           1U

/* All 2.4 GHz channels (11–26) */
#define ZB_ALL_CHANNELS_MASK     0x07FFF800U

/* ZCL Basic cluster string attributes — ZCL strings are length-prefixed (1 byte). */
/* Format: [length_byte][data_bytes...] */
static const char s_manufacturer[] = "\x0B" "DFRobot-DIY";  /* len=11 */
static const char s_model_id[]     = "\x0E" "DFR-SoilSensor"; /* len=14 */

/* Joined flag, set by signal handler, polled by zigbee_reporter_wait_ready(). */
static volatile bool s_joined = false;

/* ============================================================
 * Signal handler
 * ============================================================ */

static bool zb_signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(signal);
    const ezb_bdb_signal_simple_params_t *params = NULL;

    switch (signal_type) {
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        ESP_LOGI(TAG, "First start — initiating network steering");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        break;

    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        params = (const ezb_bdb_signal_simple_params_t *)ezb_app_signal_get_params(signal);
        if (params && params->status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Reboot — already joined, short addr 0x%04x",
                     (unsigned)ezb_get_short_address());
            s_joined = true;
        } else {
            ESP_LOGW(TAG, "Reboot but not on network — starting steering");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }
        break;

    case EZB_BDB_SIGNAL_STEERING:
        params = (const ezb_bdb_signal_simple_params_t *)ezb_app_signal_get_params(signal);
        if (params && params->status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "BDB steering complete — joined network, PAN 0x%04x, short addr 0x%04x",
                     (unsigned)ezb_get_panid(),
                     (unsigned)ezb_get_short_address());
            s_joined = true;
        } else {
            uint8_t status = params ? params->status : 0xFF;
            ESP_LOGW(TAG, "BDB steering failed (status=%u)", (unsigned)status);
        }
        break;

    default:
        /* Not handled here — let other handlers (if any) process it. */
        return false;
    }

    return true;
}

/* ============================================================
 * Zigbee task — runs the stack main loop
 * ============================================================ */

static void zb_task(void *arg)
{
    (void)arg;

    /* Start the stack without autostart — we handle commissioning via
     * the signal handler ourselves (first start or reboot cases). */
    esp_err_t err = esp_zigbee_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    /* Blocks until the stack is deinitialised (never returns in normal operation). */
    esp_zigbee_launch_mainloop();

    /* Should not reach here. */
    vTaskDelete(NULL);
}

/* ============================================================
 * Build the minimal endpoint (Basic + Identify)
 * ============================================================ */

static esp_err_t register_endpoint(void)
{
    /* ---- Basic cluster (server) ---- */
    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version  = 3,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_BATTERY, /* 0x03 */
    };
    ezb_zcl_cluster_desc_t basic_cluster =
        ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (basic_cluster == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create Basic cluster");
        return ESP_FAIL;
    }

    /* Add ManufacturerName (0x0004) and ModelIdentifier (0x0005) */
    ezb_err_t ezb_err;
    ezb_err = ezb_zcl_basic_cluster_desc_add_attr(
        basic_cluster, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer);
    if (ezb_err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Failed to add ManufacturerName attr (%d)", (int)ezb_err);
    }
    ezb_err = ezb_zcl_basic_cluster_desc_add_attr(
        basic_cluster, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model_id);
    if (ezb_err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Failed to add ModelIdentifier attr (%d)", (int)ezb_err);
    }

    /* ---- Identify cluster (server) ---- */
    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    ezb_zcl_cluster_desc_t identify_cluster =
        ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER);
    if (identify_cluster == EZB_INVALID_ZCL_CLUSTER_DESC) {
        ESP_LOGE(TAG, "Failed to create Identify cluster");
        ezb_zcl_free_cluster_desc(basic_cluster);
        return ESP_FAIL;
    }

    /* ---- Endpoint descriptor ---- */
    ezb_af_ep_config_t ep_cfg = {
        .ep_id             = ZB_ENDPOINT_ID,
        .app_profile_id    = ZB_HA_PROFILE_ID,
        .app_device_id     = ZB_HA_DEVICE_ID_GENERIC,
        .app_device_version = 0,
        .reserved           = 0,
    };
    ezb_af_ep_desc_t ep_desc = ezb_af_create_endpoint_desc(&ep_cfg);
    if (ep_desc == EZB_INVALID_AF_EP_DESC) {
        ESP_LOGE(TAG, "Failed to create endpoint descriptor");
        ezb_zcl_free_cluster_desc(basic_cluster);
        ezb_zcl_free_cluster_desc(identify_cluster);
        return ESP_FAIL;
    }

    /* Add clusters to endpoint */
    if (ezb_af_endpoint_add_cluster_desc(ep_desc, basic_cluster) != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to add Basic cluster to endpoint");
        ezb_af_free_endpoint_desc(ep_desc);
        return ESP_FAIL;
    }
    if (ezb_af_endpoint_add_cluster_desc(ep_desc, identify_cluster) != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to add Identify cluster to endpoint");
        ezb_af_free_endpoint_desc(ep_desc);
        return ESP_FAIL;
    }

    /* ---- Device descriptor ---- */
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    if (dev_desc == EZB_INVALID_AF_DEVICE_DESC) {
        ESP_LOGE(TAG, "Failed to create device descriptor");
        ezb_af_free_endpoint_desc(ep_desc);
        return ESP_FAIL;
    }

    if (ezb_af_device_add_endpoint_desc(dev_desc, ep_desc) != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to add endpoint to device descriptor");
        ezb_af_free_device_desc(dev_desc);
        return ESP_FAIL;
    }

    if (ezb_af_device_desc_register(dev_desc) != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "Failed to register device descriptor");
        ezb_af_free_device_desc(dev_desc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Endpoint %u registered (Basic + Identify)", ZB_ENDPOINT_ID);
    return ESP_OK;
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t zigbee_reporter_init(void)
{
    ESP_LOGI(TAG, "Initialising Zigbee end-device");

    /* Build the zigbee config: end-device, native radio, zb_storage partition. */
    esp_zigbee_config_t zb_cfg = {
        .device_config = {
            .device_type         = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config = {
                /* 64-minute end-device timeout before parent drops child table entry */
                .ed_timeout  = EZB_NWK_ED_TIMEOUT_64MIN,
                /* Keep-alive interval: send a data poll every 3 000 ms */
                .keep_alive  = 3000,
            },
        },
        .platform_config = {
            .storage_partition_name = "zb_storage",
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };

    esp_err_t err = esp_zigbee_init(&zb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zigbee_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Set channel mask to all 2.4 GHz channels (11–26). */
    ezb_err_t ezb_err = ezb_set_channel_mask(ZB_ALL_CHANNELS_MASK);
    if (ezb_err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "ezb_set_channel_mask returned %d", (int)ezb_err);
    }

    /* Register our application signal handler. */
    ezb_err = ezb_app_signal_add_handler(zb_signal_handler);
    if (ezb_err != EZB_ERR_NONE) {
        ESP_LOGE(TAG, "ezb_app_signal_add_handler failed: %d", (int)ezb_err);
        return ESP_FAIL;
    }

    /* Register minimal endpoint (Basic + Identify). */
    err = register_endpoint();
    if (err != ESP_OK) {
        return err;
    }

    /* Create the Zigbee stack task.
     * Stack size 4096 and priority 5 mirror the ESP-IDF HA examples. */
    BaseType_t ret = xTaskCreate(zb_task, "zb_task", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Zigbee task created — joining async");
    return ESP_OK;
}

bool zigbee_reporter_wait_ready(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    const uint32_t poll_ms = 100;

    while (!s_joined && elapsed < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        elapsed += poll_ms;
    }

    return s_joined;
}

esp_err_t zigbee_reporter_report(float soil_pct, float battery_v, float battery_pct)
{
    /* Stub — real attribute reporting wired in Task 4. */
    ESP_LOGI(TAG, "report (stub): soil=%.1f%% batt=%.2fV (%.0f%%)",
             soil_pct, battery_v, battery_pct);
    return ESP_OK;
}

#endif /* USE_ZIGBEE */
