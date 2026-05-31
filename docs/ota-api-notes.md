# esp-zigbee-lib 1.6.x OTA Client API Notes

Confirmed from the installed esp32c6 headers at:
`~/.platformio/packages/framework-arduinoespressif32-libs/esp32c6/include/espressif__esp-zigbee-lib/include/`

Library version: **1.6.3** (`ESP_ZB_VER_MAJOR=1`, `_MINOR=6`, `_PATCH=3`)

---

## 1. `esp_zb_ota_cluster_cfg_t`

Defined in `esp_zigbee_type.h`:

```c
typedef struct esp_zb_ota_cluster_cfg_s {
    uint32_t ota_upgrade_file_version;        /*!< File version of running firmware image */
    uint16_t ota_upgrade_manufacturer;        /*!< Manufacturer code */
    uint16_t ota_upgrade_image_type;          /*!< Image type */
    uint16_t ota_min_block_reque;             /*!< Delay between Image Block Request commands (ms) */
    uint32_t ota_upgrade_file_offset;         /*!< Current location in OTA upgrade image */
    uint32_t ota_upgrade_downloaded_file_ver; /*!< File version of downloaded image */
    esp_zb_ieee_addr_t ota_upgrade_server_id; /*!< Address of the upgrade server */
    uint8_t  ota_image_upgrade_status;        /*!< Image upgrade status of the client device */
} esp_zb_ota_cluster_cfg_t;
```

**Note:** There are NO hardware version or stack version fields in this struct (contrary to plan assumptions). Those are tracked as separate ZCL attributes (see attribute IDs below) but are not part of `esp_zb_ota_cluster_cfg_t`.

---

## 2. Cluster Creation and Attribute Addition

### `esp_zb_ota_cluster_create`

Declared in `esp_zigbee_cluster.h`:

```c
esp_zb_attribute_list_t *esp_zb_ota_cluster_create(esp_zb_ota_cluster_cfg_t *ota_cfg);
```

Creates a standard OTA cluster attribute list containing only the mandatory attributes.
Returns a pointer to an `esp_zb_attribute_list_t`.

### `esp_zb_ota_cluster_add_attr`

Declared in `esp_zigbee_attribute.h`:

```c
esp_err_t esp_zb_ota_cluster_add_attr(esp_zb_attribute_list_t *attr_list,
                                      uint16_t attr_id,
                                      void *value_p);
```

Adds an additional attribute (by ZCL attribute ID) to an existing OTA attribute list.

---

## 3. Adding the OTA Cluster to a Cluster List

Declared in `esp_zigbee_cluster.h`:

```c
esp_err_t esp_zb_cluster_list_add_ota_cluster(esp_zb_cluster_list_t *cluster_list,
                                              esp_zb_attribute_list_t *attr_list,
                                              uint8_t role_mask);
```

- `role_mask` is from `esp_zb_zcl_cluster_role_t`:
  - `ESP_ZB_ZCL_CLUSTER_SERVER_ROLE = 0x01U`
  - `ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE = 0x02U`
- For OTA client use: `ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE`

An update variant also exists:
```c
esp_err_t esp_zb_cluster_list_update_ota_cluster(esp_zb_cluster_list_t *cluster_list,
                                                 esp_zb_attribute_list_t *attr_list,
                                                 uint8_t role_mask);
```

---

## 4. OTA Client Query Functions

Both declared in `esp_zigbee_ota.h`:

### `esp_zb_ota_upgrade_client_query_interval_set`

```c
esp_err_t esp_zb_ota_upgrade_client_query_interval_set(uint8_t endpoint, uint16_t interval);
```

- `endpoint`: the endpoint identifier of the OTA Upgrade client
- `interval`: query interval **in minutes**
- Returns `ESP_OK` / `ESP_FAIL`

### `esp_zb_ota_upgrade_client_query_image_req`

```c
esp_err_t esp_zb_ota_upgrade_client_query_image_req(uint16_t server_ep, uint8_t server_addr);
```

**WARNING — argument order mismatch from plan:** The header has `(uint16_t server_ep, uint8_t server_addr)` — endpoint comes first, short address comes second, and the types are `uint16_t`/`uint8_t` (not `uint16_t`/`uint8_t` in reversed order as the plan assumed).

The docstring says:
> `server_addr`: The short address of the OTA upgrade server that the client expects to query
> `server_ep`: The endpoint identifier of the OTA upgrade server

So: `esp_zb_ota_upgrade_client_query_image_req(server_endpoint, server_short_addr)`.

### `esp_zb_ota_upgrade_client_query_image_stop`

```c
esp_err_t esp_zb_ota_upgrade_client_query_image_stop(void);
```

Stops the image query.

---

## 5. OTA Value Callback

### Registration

Declared in `esp_zigbee_core.h`:

```c
typedef esp_err_t (*esp_zb_core_action_callback_t)(esp_zb_core_action_callback_id_t callback_id,
                                                   const void *message);

void esp_zb_core_action_handler_register(esp_zb_core_action_callback_t cb);
```

Register a single global handler; dispatch on `callback_id`.

### Callback ID

```c
ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID = 0x0004,  /*!< Upgrade OTA, refer to esp_zb_zcl_ota_upgrade_value_message_t */
```

Related OTA server-side IDs (not needed for client):
```c
ESP_ZB_CORE_OTA_UPGRADE_SRV_STATUS_CB_ID       = 0x0005,
ESP_ZB_CORE_OTA_UPGRADE_SRV_QUERY_IMAGE_CB_ID  = 0x0006,
ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID = 0x0031,
```

### Message Struct

Defined in `zcl/esp_zigbee_zcl_command.h`:

```c
typedef struct esp_zb_zcl_ota_upgrade_value_message_s {
    esp_zb_device_cb_common_info_t info;             /*!< Common information for Zigbee device callback */
    esp_zb_zcl_ota_upgrade_status_t upgrade_status;  /*!< The update status */
    esp_zb_ota_file_header_t ota_header;             /*!< Basic OTA upgrade information */
    uint16_t payload_size;                           /*!< OTA payload size */
    uint8_t *payload;                                /*!< OTA payload */
} esp_zb_zcl_ota_upgrade_value_message_t;
```

### `esp_zb_ota_file_header_t`

Defined in `esp_zigbee_ota.h`:

```c
typedef struct esp_zb_ota_file_header_s {
    uint16_t manufacturer_code;   /*!< OTA header manufacturer code */
    uint16_t image_type;          /*!< Image type value */
    uint32_t file_version;        /*!< Release and build number */
    uint32_t image_size;          /*!< Total image size in bytes */
    uint16_t field_control;       /*!< Indicates whether additional optional info is present */
    esp_zb_ota_file_optional_t optional; /*!< Optional header fields */
} esp_zb_ota_file_header_t;
```

### Return Convention

The callback returns `esp_err_t`. Return `ESP_OK` to accept/continue; non-OK return causes the stack to abort/reject the operation.

---

## 6. `ESP_ZB_ZCL_OTA_UPGRADE_STATUS_*` Enum

Defined in `zcl/esp_zigbee_zcl_ota.h` as `esp_zb_zcl_ota_upgrade_status_t`:

```c
typedef enum {
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START            = 0x0000, /*!< Starts OTA upgrade */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY            = 0x0001, /*!< Checks manufacturer/image type etc — last step before actual upgrade */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE          = 0x0002, /*!< Process image block */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH           = 0x0003, /*!< OTA upgrade completed */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT            = 0x0004, /*!< OTA upgrade aborted */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK            = 0x0005, /*!< Downloading finished, do additional checks before upgrade end request */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK               = 0x0006, /*!< OTA upgrade end response is ok */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR            = 0x0007, /*!< OTA upgrade returned error code */
    ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_NORMAL     = 0x0008, /*!< Accepted new image */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_BUSY             = 0x0009, /*!< Another download in progress, deny new image */
    ESP_ZB_ZCL_OTA_UPGRADE_STATUS_SERVER_NOT_FOUND = 0x000A, /*!< OTA Upgrade server not found */
} esp_zb_zcl_ota_upgrade_status_t;
```

**Note:** Value `0x0008` is named `ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_NORMAL` (not `STATUS_IMAGE_NORMAL`). The plan did not reference this value, but it exists.

---

## 7. OTA ZCL Attribute IDs

For reference when calling `esp_zb_ota_cluster_add_attr`:

```c
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ID                     = 0x0000
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_OFFSET_ID                = 0x0001
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_FILE_VERSION_ID               = 0x0002  /* CurrentFileVersion */
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_STACK_VERSION_ID              = 0x0003  /* CurrentZigbeeStackVersion */
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_ID    = 0x0004
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_DOWNLOADED_STACK_VERSION_ID   = 0x0005
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_STATUS_ID               = 0x0006
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_MANUFACTURE_ID                = 0x0007
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_TYPE_ID                 = 0x0008
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_MIN_BLOCK_REQUE_ID            = 0x0009
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_IMAGE_STAMP_ID                = 0x000a
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_UPGRADE_ACTIVATION_POLICY_ID  = 0x000b
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_UPGRADE_TIMEOUT_POLICY_ID     = 0x000c
/* Custom/private attributes (not in ZCL spec): */
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID            = 0xfff3
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID                = 0xfff2
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID                = 0xfff1  /* type: esp_zb_zcl_ota_upgrade_client_variable_t */
ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_DATA_ID                = 0xfff0  /* type: esp_zb_zcl_ota_upgrade_server_variable_t */
```

---

## 8. Burst Mode / Fast-Poll API Availability

### `esp_zb_set_rx_on_when_idle` — AVAILABLE

Declared in `nwk/esp_zigbee_nwk.h`:

```c
void esp_zb_set_rx_on_when_idle(bool rx_on);
bool esp_zb_get_rx_on_when_idle(void);
```

### `esp_zb_zdo_pim_set_long_poll_interval` — NOT PRESENT

The plan (Task 11) assumed `esp_zb_zdo_pim_set_long_poll_interval(...)` for fast-polling. This function **does not exist** in esp-zigbee-lib 1.6.3 headers. No equivalent poll-interval setter was found in any of the `nwk/`, `zdo/`, or main include headers.

**Consequence for Task 11:** Drop the two `esp_zb_zdo_pim_set_long_poll_interval` calls. Burst mode can still rely on `esp_zb_set_rx_on_when_idle(true)` + disabling light sleep via `esp_zb_sleep_enable(false)` to keep blocks flowing. The `zed_cfg.keep_alive` parameter (set at init time) governs poll rate; it cannot be changed at runtime via this API.

---

## 9. Deviations from Plan Assumptions

| Plan assumption | Reality (header) | Action |
|---|---|---|
| `esp_zb_ota_cluster_cfg_t` has hardware/stack version fields | Not present — only 8 fields listed above | Fields not needed; omit from initialization |
| `esp_zb_ota_upgrade_client_query_image_req(server_addr, server_ep)` — addr first, ep second | Header is `(uint16_t server_ep, uint8_t server_addr)` — **ep first, addr second**, with reversed types | Swap args in Task 9: `esp_zb_ota_upgrade_client_query_image_req(server_ep, server_short_addr)` |
| `esp_zb_zdo_pim_set_long_poll_interval(...)` exists | NOT in 1.6.3 | Drop from Task 11; use rx-on + sleep-disable instead |
| `ESP_ZB_ZCL_OTA_UPGRADE_STATUS_*` ordering: START=0, RECEIVE=1, APPLY=2, ... | Actual ordering: START=0, **APPLY=1**, **RECEIVE=2**, FINISH=3, ABORT=4, CHECK=5 | Update the switch-case dispatch in Task 10 — APPLY fires before RECEIVE in state ordering |
| Plan did not mention `ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_NORMAL = 0x0008` | Present in enum | Handle in default case or explicitly in Task 10 |

---

## 10. Useful Default Macro Values

```c
ESP_ZB_OTA_UPGRADE_MANUFACTURER_CODE_DEF_VALUE     = 0x131B
ESP_ZB_OTA_UPGRADE_IMAGE_TYPE_DEF_VALUE            = 0xffbf  /* "wildcard" */
ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF       = (24*60) /* 1440 minutes = 24 hours */
ESP_ZB_ZCL_OTA_UPGRADE_FILE_VERSION_DEF_VALUE      = 0xffffffff
ESP_ZB_ZCL_OTA_UPGRADE_DOWNLOADED_FILE_VERSION_DEF_VALUE = 0xffffffff
ESP_ZB_ZCL_OTA_UPGRADE_STACK_VERSION_DEF_VALUE     = 0xffff
```
