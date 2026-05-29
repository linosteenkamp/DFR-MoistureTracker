/**
 * @file zigbee_reporter.c
 * @brief Zigbee end-device stack init + BDB join + sensor cluster reporting.
 *        esp-zigbee-lib 1.6.x native esp_zb_* API.
 *
 * The entire body is wrapped in #ifdef USE_ZIGBEE so that the WiFi build
 * (no -DUSE_ZIGBEE) compiles this file as empty — no undefined references.
 */

#ifdef USE_ZIGBEE

#include "zigbee_reporter.h"
#include "zigbee_encode.h"

/* Classic esp_zb_* API, native to esp-zigbee-lib 1.6.x (headers at the
 * include root; no compat/ prefix). Matched pair with esp-zboss-lib 1.6.x. */
#include "esp_zigbee_core.h"         /* esp_zb_cfg_t, esp_zb_init,
                                        esp_zb_lock_acquire/release,
                                        ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK */
#include "platform/esp_zigbee_platform.h"  /* esp_zb_platform_config_t */
#include "nwk/esp_zigbee_nwk.h"     /* ESP_ZB_DEVICE_TYPE_ED, ESP_ZB_ED_AGING_TIMEOUT_64MIN */
#include "bdb/esp_zigbee_bdb_commissioning.h" /* esp_zb_bdb_start_top_level_commissioning */
#include "zdo/esp_zigbee_zdo_common.h"        /* esp_zb_app_signal_t, signal type enum */
#include "esp_zigbee_cluster.h"      /* esp_zb_*_cluster_create,
                                        esp_zb_zcl_cluster_list_create,
                                        esp_zb_cluster_list_add_*,
                                        esp_zb_cluster_list_add_custom_cluster */
#include "esp_zigbee_attribute.h"    /* esp_zb_basic_cluster_add_attr,
                                        esp_zb_power_config_cluster_add_attr,
                                        esp_zb_zcl_attr_list_create,
                                        esp_zb_custom_cluster_add_custom_attr,
                                        esp_zb_zcl_set_attribute_val */
#include "esp_zigbee_endpoint.h"     /* esp_zb_ep_list_create, esp_zb_ep_list_add_ep */
#include "zcl/esp_zigbee_zcl_common.h"   /* ESP_ZB_AF_HA_PROFILE_ID,
                                            ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
                                            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                                            ESP_ZB_ZCL_ATTR_TYPE_U16,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                                            ESP_ZB_ZCL_ATTR_ACCESS_REPORTING */
#include "zcl/esp_zigbee_zcl_basic.h"    /* ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID/MODEL_IDENTIFIER_ID */
#include "zcl/esp_zigbee_zcl_power_config.h" /* ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID (0x0020),
                                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID (0x0021) */
#include "zcl/esp_zigbee_zcl_humidity_meas.h" /* ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID (soil via 0x0405) */
#include "zcl/esp_zigbee_zcl_core.h"     /* esp_zb_device_register */
#include "zcl/esp_zigbee_zcl_command.h"  /* esp_zb_zcl_report_attr_cmd_t, esp_zb_zcl_report_attr_cmd_req */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aps/esp_zigbee_aps.h"  /* ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT */
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
 * File-scope attribute value storage.
 *
 * The ZCL attribute tables store POINTERS into these variables.
 * They must remain valid for the lifetime of the stack (i.e.
 * forever), so they are module-level statics — NOT stack locals.
 * ============================================================ */

/* Power Configuration cluster (0x0001) */
static uint8_t  s_batt_voltage = 0;    /* 0x0020: BatteryVoltage, uint8, units of 100mV */
static uint8_t  s_batt_pct     = 0;    /* 0x0021: BatteryPercentageRemaining, uint8, units of 0.5% */

/* Soil moisture, carried on the Relative Humidity Measurement cluster (0x0405).
 * MeasuredValue, uint16, units of 0.01% — same format as soil moisture %. */
static uint16_t s_soil_measured = 0;

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

    /* Keep the receiver on when idle so the coordinator can reliably interview
     * the device (ZDO Active_EP_req etc.) and reach it during its awake window.
     * A pure sleepy ED (rx-off) makes z2m's interview time out with
     * "can not get active endpoints". Power cost is bounded because the deep-sleep
     * model (Task 6) powers the radio down entirely between wakes; rx-on only
     * applies while the device is already awake. */
    esp_zb_set_rx_on_when_idle(true);

    /* ---- Basic cluster ---- */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = 8,
        .power_source = 3,  /* 0x03 = battery */
    };
    esp_zb_attribute_list_t *basic_attrs = esp_zb_basic_cluster_create(&basic_cfg);

    /* Add optional string attributes: ManufacturerName and ModelIdentifier.
     * The cast discards const — the API takes void*, but does not mutate the string. */
    esp_zb_basic_cluster_add_attr(basic_attrs,
                                  ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                  (void *)MANUF_NAME);
    esp_zb_basic_cluster_add_attr(basic_attrs,
                                  ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                  (void *)MODEL_ID);

    /* ---- Identify cluster ---- */
    esp_zb_identify_cluster_cfg_t identify_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *identify_attrs = esp_zb_identify_cluster_create(&identify_cfg);

    /* ---- Power Configuration cluster (0x0001) ---- */
    /* The default cfg creates MainsVoltage+MainsFrequency; we use the helper then
     * add battery attrs explicitly.  A zeroed cfg gives sensible defaults (mains=0,
     * freq=0) which is fine since we only care about the battery sub-attributes. */
    esp_zb_power_config_cluster_cfg_t power_cfg;
    memset(&power_cfg, 0, sizeof(power_cfg));
    esp_zb_attribute_list_t *power_attrs = esp_zb_power_config_cluster_create(&power_cfg);

    /* BatteryVoltage (0x0020) — uint8, read-only + reportable */
    esp_zb_power_config_cluster_add_attr(power_attrs,
                                         ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                         &s_batt_voltage);
    /* BatteryPercentageRemaining (0x0021) — uint8, read-only + reportable */
    esp_zb_power_config_cluster_add_attr(power_attrs,
                                         ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                         &s_batt_pct);

    /* ---- Soil moisture via standard Relative Humidity Measurement cluster (0x0405) ----
     * A custom 0x0408 cluster asserts in the stack's ZCL general-command path on this
     * SDK version. Soil moisture % and relative humidity % share the same wire format
     * (uint16, 0.01% units, 0..10000), so we report soil via the native, stack-supported
     * humidity cluster. The zigbee2mqtt converter relabels it to soil_moisture. */
    esp_zb_humidity_meas_cluster_cfg_t humidity_cfg = {
        .measured_value = 0,
        .min_value      = 0,
        .max_value      = 10000,
    };
    esp_zb_attribute_list_t *humidity_attrs = esp_zb_humidity_meas_cluster_create(&humidity_cfg);

    /* ---- Assemble cluster list ---- */
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(clusters, basic_attrs,
                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(clusters, identify_attrs,
                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_power_config_cluster(clusters, power_attrs,
                                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_humidity_meas_cluster(clusters, humidity_attrs,
                                                  ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* ---- Register endpoint ---- */
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

/* Send a single attribute report to the coordinator (short addr 0x0000, ep 1).
 * Must be called with the Zigbee stack lock held. */
static void report_attr(uint16_t cluster_id, uint16_t attr_id)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000U,   /* coordinator */
            .dst_endpoint          = 1U,
            .src_endpoint          = APP_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID    = cluster_id,
        .attributeID  = attr_id,
    };
    esp_zb_zcl_report_attr_cmd_req(&cmd);
}

esp_err_t zigbee_reporter_report(float soil_pct, float battery_v, float battery_pct)
{
    /* Encode float values into ZCL wire formats. */
    uint16_t soil = zigbee_encode_soil_pct(soil_pct);
    uint8_t  volt = zigbee_encode_batt_voltage(battery_v);
    uint8_t  pct  = zigbee_encode_batt_pct(battery_pct);

    /* Update the static storage that the cluster's attribute table points to.
     * Do this BEFORE set_attribute_val so both the pointer and the set value agree. */
    s_soil_measured = soil;
    s_batt_voltage  = volt;
    s_batt_pct      = pct;

    /* Take the Zigbee stack lock before touching ZCL data structures.
     * esp_zb_lock_acquire returns true if the lock was acquired. */
    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to acquire Zigbee lock");
        return ESP_FAIL;
    }

    /* Update the ZCL attribute store. The stack auto-reports reportable attrs
     * once the coordinator has configured reporting (post-interview). */

    /* --- Soil moisture via Relative Humidity Measurement cluster (0x0405) --- */
    esp_zb_zcl_set_attribute_val(APP_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
                                 &s_soil_measured,
                                 false);

    /* --- Power Configuration cluster (0x0001) --- */
    esp_zb_zcl_set_attribute_val(APP_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID,
                                 &s_batt_voltage,
                                 false);

    esp_zb_zcl_set_attribute_val(APP_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID,
                                 &s_batt_pct,
                                 false);

    /* Explicitly push reports to the coordinator. This is required for the
     * deep-sleep model: the radio is off between wakes, so z2m cannot poll a
     * sleeping device — the device must send. These are all STANDARD clusters
     * (Humidity 0x0405, Power Config 0x0001); the earlier assert was specific to
     * the custom 0x0408 cluster, which we no longer use. */
    report_attr(ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
                ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID);
    report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID);
    report_attr(ESP_ZB_ZCL_CLUSTER_ID_POWER_CONFIG,
                ESP_ZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID);

    esp_zb_lock_release();

    ESP_LOGI(TAG, "reported soil=%u(%.1f%%) volt=%u pct=%u",
             soil, soil_pct, volt, pct);

    return ESP_OK;
}

#endif /* USE_ZIGBEE */
