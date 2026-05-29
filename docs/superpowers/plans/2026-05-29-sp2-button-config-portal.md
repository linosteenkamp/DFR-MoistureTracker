# SP2 — Button-Triggered Config Portal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the Zigbee firmware, a GPIO7 press reboots the device into a SoftAP config portal (soil calibration + sensor name, no WiFi-station setup); the sensor name is persisted, shown on the e-paper, exposed over Zigbee via Basic LocationDescription (0x0010), and surfaced by the z2m converter as a `label` MQTT payload field for Node-RED.

**Architecture:** Reuses the SP1 "reboot-into-mode" pattern. A GPIO7 ISR (also a light-sleep wake source) signals a trigger task that latches an `RTC_DATA_ATTR` flag and `esp_restart()`s. On boot, `app_main()` sees the flag (before the brownout gate), clears it, and runs the existing `config_portal_run()` with a Zigbee-specific landing page, then restarts back into normal Zigbee operation. The name rides the Basic cluster's standard LocationDescription attribute (custom clusters assert in this SDK).

**Tech Stack:** ESP-IDF 5.5 (PlatformIO, `framework=espidf`), esp-zigbee-lib 1.6.x (`esp_zb_*`), esp_http_server SoftAP portal, zigbee2mqtt external converter (JS).

**Testing reality:** Per `CLAUDE.md`, this firmware has no automated tests — validation is `pio run` builds plus on-device serial/z2m observation. The user flashes hardware and reports results. Each task therefore ends with a build gate, a commit, and explicit on-device verification steps rather than unit tests. Run all builds with the Zigbee env:
`pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
and the WiFi default with `pio run` to confirm no regressions.

**Branch:** `feat/sp1-zigbee-transport` (continues SP1; SP2 builds on it).

---

## File Structure

- `src/main.c` — adds the GPIO7 ISR + light-sleep wake arming, the trigger task, the `RTC_DATA_ATTR` config flag, the `app_main` config-mode fork, and the `zigbee_reporter_set_location()` call. All new code under `#ifdef USE_ZIGBEE`.
- `src/zigbee_reporter.c` / `include/zigbee_reporter.h` — adds the LocationDescription attribute to the Basic cluster and a `zigbee_reporter_set_location()` setter.
- `src/config_portal.c` — adds a `#ifdef USE_ZIGBEE` landing-page variant plus `/name` (GET+POST) and `/reboot` handlers; no change to calibration handlers.
- `z2m/dfr_soil_moisture.js` — exposes `label` from `genBasic.locationDesc`.
- `DEVELOPER_GUIDE.md`, `CLAUDE.md` — document the button → config portal flow and the `label` field.

---

## Task 1: Button entry — ISR, light-sleep wake, RTC flag, restart (risk spike first)

Validates the one real unknown — that a GPIO7 press reliably wakes the CPU from `esp_zb` managed light sleep and fires the ISR — while building the actual entry mechanism. The portal itself is wired in Task 3; here the boot-side fork only logs, so we can confirm the press→reboot→flag loop in isolation.

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Add the RTC config-request flag**

In `src/main.c`, directly below the existing `s_low_battery_shown` declaration (currently `src/main.c:71`), add:

```c
// Set by the GPIO7 trigger task before esp_restart(); read once early in
// app_main() to enter config-portal mode. RTC memory survives the soft reset.
RTC_DATA_ATTR static bool s_config_requested = false;
```

- [ ] **Step 2: Add the ISR, trigger task, and arming helper**

In `src/main.c`, add this block immediately above the `#ifdef USE_ZIGBEE` sampling section (just before the `zb_sample_sensors` definition, currently `src/main.c:465`):

```c
#ifdef USE_ZIGBEE
// --- GPIO7 config-portal entry (Zigbee build) ---------------------------------
// The Zigbee build runs managed light sleep (CPU mostly halted between radio
// events), so the deep-sleep GPIO-wake path never runs. Instead GPIO7 is armed
// as a light-sleep wake source with a falling-edge ISR. A press wakes the CPU,
// the ISR signals s_config_btn_sem, and config_trigger_task latches the RTC flag
// and restarts into config mode. esp_restart() is illegal in ISR context, hence
// the task hop.
static SemaphoreHandle_t s_config_btn_sem = NULL;

static void IRAM_ATTR config_btn_isr(void *arg)
{
    (void)arg;
    BaseType_t hp_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_config_btn_sem, &hp_woken);
    if (hp_woken) {
        portYIELD_FROM_ISR();
    }
}

static void config_trigger_task(void *pv)
{
    (void)pv;
    for (;;) {
        if (xSemaphoreTake(s_config_btn_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Debounce: require the line to still be low ~40 ms after the edge.
        vTaskDelay(pdMS_TO_TICKS(40));
        if (gpio_get_level(GPIO_NUM_7) != 0) {
            continue;   // bounce/noise — ignore
        }
        ESP_LOGI(TAG, "GPIO7 pressed — rebooting into config mode");
        s_config_requested = true;
        vTaskDelay(pdMS_TO_TICKS(50));   // let the log flush
        esp_restart();
    }
}

static void setup_config_button(void)
{
    s_config_btn_sem = xSemaphoreCreateBinary();
    if (s_config_btn_sem == NULL) {
        ESP_LOGW(TAG, "config button sem alloc failed — button disabled");
        return;
    }

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << GPIO_NUM_7),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn);

    // Wake the CPU out of managed light sleep when GPIO7 goes low.
    gpio_wakeup_enable(GPIO_NUM_7, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_7, config_btn_isr, NULL);

    xTaskCreate(config_trigger_task, "cfg_btn", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "GPIO7 config button armed (light-sleep wake + ISR)");
}
#endif /* USE_ZIGBEE */
```

- [ ] **Step 3: Add the boot-side config-mode fork (stub for now)**

In `src/main.c`, immediately after the `#endif` that closes the WiFi-only portal block (currently `src/main.c:602`) and **before** the zero-load battery sample comment (currently `src/main.c:604`), add:

```c
#ifdef USE_ZIGBEE
    // Config-portal mode takes priority over the brownout gate: a deliberate
    // button press should always reach config (radio is off, so power is modest).
    // Clear the flag first so a crash mid-portal returns to normal operation.
    if (s_config_requested) {
        s_config_requested = false;
        ESP_LOGI(TAG, "Config requested — portal wired in Task 3");
        // Task 3 replaces this stub with:
        //   display + config_portal_run() + esp_restart();
    }
#endif
```

- [ ] **Step 4: Arm the button in the Zigbee path**

In `src/main.c`, in the Zigbee transport block, immediately after the `device_id_buffer` load (the closing `}` currently at `src/main.c:634`) and before the display-task setup, add:

```c
    setup_config_button();
```

- [ ] **Step 5: Build the Zigbee env**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: `SUCCESS`. (Ignore clangd LSP errors about `freertos/FreeRTOS.h` etc. — they are known false positives; only the `pio` result matters.)

- [ ] **Step 6: Build the WiFi env (no regression)**

Run: `pio run`
Expected: `SUCCESS` (the new code is all under `#ifdef USE_ZIGBEE`).

- [ ] **Step 7: Commit**

```bash
git add src/main.c
git commit -m "zigbee: GPIO7 config-button entry (ISR + light-sleep wake + RTC flag)"
```

- [ ] **Step 8: On-device verification (user flashes)**

Flash + monitor: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor`
Verify, in order:
1. Boot logs `GPIO7 config button armed (light-sleep wake + ISR)` and the device joins Zigbee as normal.
2. After it settles into managed light sleep, press GPIO7 once. Expected: `GPIO7 pressed — rebooting into config mode`, the device reboots, and the next boot logs `Config requested — portal wired in Task 3`, then resumes normal Zigbee operation.
3. If the press does NOT wake/reboot the device, the light-sleep wake source is the problem — STOP and report before proceeding (this is the de-risk gate for the whole plan).

---

## Task 2: Expose the sensor name over Zigbee (Basic LocationDescription 0x0010)

Adds the standard, writable Basic-cluster string attribute that carries the device-set name, set from the NVS `device_id`. z2m reads it in Task 4.

**Files:**
- Modify: `include/zigbee_reporter.h`
- Modify: `src/zigbee_reporter.c`
- Modify: `src/main.c`

- [ ] **Step 1: Declare the setter in the header**

In `include/zigbee_reporter.h`, directly below the `zigbee_reporter_set_report_done_cb` declaration, add:

```c
/* Set the Basic-cluster LocationDescription (0x0010) string, surfaced by the
 * z2m converter as the `label` payload field. Must be called before
 * zigbee_reporter_init() (the value is read when the cluster is created). Names
 * longer than 16 chars are truncated (ZCL char-string limit for this attr). */
void zigbee_reporter_set_location(const char *name);
```

- [ ] **Step 2: Add the location storage + setter in the reporter**

In `src/zigbee_reporter.c`, add a file-scope buffer next to the other attribute storage (immediately after the `s_soil_measured` declaration, currently `src/zigbee_reporter.c:105`):

```c
/* Basic cluster LocationDescription (0x0010): ZCL character string —
 * byte 0 is the length, followed by up to 16 chars. Set via
 * zigbee_reporter_set_location() before the cluster is created. */
static char s_location_zcl[1 + 16 + 1] = {0};
```

Then add the setter next to the other setters (after `zigbee_reporter_set_report_done_cb`, currently around `src/zigbee_reporter.c:78`):

```c
void zigbee_reporter_set_location(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n > 16) {
        n = 16;
    }
    s_location_zcl[0] = (char)(uint8_t)n;          /* ZCL length prefix */
    memcpy(&s_location_zcl[1], name ? name : "", n);
    s_location_zcl[1 + n] = '\0';
}
```

- [ ] **Step 3: Add the attribute to the Basic cluster**

In `src/zigbee_reporter.c`, in `esp_zb_task()`, immediately after the `MODEL_ID` attribute is added (the `esp_zb_basic_cluster_add_attr(..., ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, ...)` call, currently `src/zigbee_reporter.c:301-303`), add:

```c
    /* LocationDescription (0x0010) — device-set sensor name, surfaced by the
     * z2m converter as `label`. Standard Basic attr (custom clusters assert in
     * this SDK). Value was populated by zigbee_reporter_set_location(). */
    esp_zb_basic_cluster_add_attr(basic_attrs,
                                  ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID,
                                  (void *)s_location_zcl);
```

- [ ] **Step 4: Set the location from main.c before init**

In `src/main.c`, in the Zigbee transport block, immediately after the `device_id_buffer` load and the `setup_config_button();` call (added in Task 1 Step 4), add:

```c
    zigbee_reporter_set_location(device_id_buffer);
```

- [ ] **Step 5: Build the Zigbee env**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: `SUCCESS`. If the compiler reports `ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID` undeclared, confirm the constant in `managed_components/espressif__esp-zigbee-lib/include/zcl/esp_zigbee_zcl_basic.h` (it is `0x0010`) and that `zcl/esp_zigbee_zcl_basic.h` is included (it is, at `src/zigbee_reporter.c:41`).

- [ ] **Step 6: Build the WiFi env (no regression)**

Run: `pio run`
Expected: `SUCCESS`.

- [ ] **Step 7: Commit**

```bash
git add include/zigbee_reporter.h src/zigbee_reporter.c src/main.c
git commit -m "zigbee: expose sensor name via Basic LocationDescription (0x0010)"
```

- [ ] **Step 8: On-device verification (user flashes)**

Flash the Zigbee env. In zigbee2mqtt, open the device → Dev console (or Exposes), read the Basic cluster (`genBasic`) `locationDesc` attribute. Expected: it returns the current device ID string (default `sensor02` until a name is set in Task 3). The `label` entity itself appears after the converter is updated in Task 4.

---

## Task 3: Config-portal Zigbee variant — name page, menu, run from app_main

Wires the actual portal into the boot fork and adds a Zigbee-only landing page (no WiFi-station fields) with a sensor-name form and a reboot action. Calibration handlers are reused unchanged.

**Files:**
- Modify: `src/config_portal.c`
- Modify: `src/main.c`

- [ ] **Step 1: Add the Zigbee landing-page menu variant**

In `src/config_portal.c`, replace the single `html_menu` definition (currently `src/config_portal.c:45-58`) with a build-conditional pair:

```c
#ifdef USE_ZIGBEE
static const char *html_menu =
    "<!DOCTYPE html><html><head><title>FireBeetle Config</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
    "a.btn{display:block;padding:14px;margin:10px 0;background:#4CAF50;color:white;"
    "text-align:center;text-decoration:none;border-radius:4px}"
    "a.btn.danger{background:#d9534f}"
    "form{margin:0}button.btn{display:block;width:100%;padding:14px;margin:10px 0;"
    "background:#337ab7;color:white;border:none;border-radius:4px;font-size:15px;cursor:pointer}"
    "</style></head>"
    "<body><div class='container'><h2>FireBeetle C6 (Zigbee)</h2>"
    "<a class='btn' href='/name'>Set Sensor Name</a>"
    "<a class='btn' href='/calibrate'>Calibrate Sensor</a>"
    "<a class='btn' href='/status'>Status</a>"
    "<form action='/reboot' method='POST'><button class='btn' type='submit'>Done \xE2\x80\x94 Reboot</button></form>"
    "<a class='btn danger' href='/factory-reset'>Factory Reset</a>"
    "</div></body></html>";
#else
static const char *html_menu =
    "<!DOCTYPE html><html><head><title>FireBeetle Config</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
    "a.btn{display:block;padding:14px;margin:10px 0;background:#4CAF50;color:white;"
    "text-align:center;text-decoration:none;border-radius:4px}"
    "a.btn.danger{background:#d9534f}</style></head>"
    "<body><div class='container'><h2>FireBeetle C6</h2>"
    "<a class='btn' href='/wifi'>WiFi &amp; Device ID</a>"
    "<a class='btn' href='/calibrate'>Calibrate Sensor</a>"
    "<a class='btn' href='/status'>Status</a>"
    "<a class='btn danger' href='/factory-reset'>Factory Reset</a>"
    "</div></body></html>";
#endif
```

- [ ] **Step 2: Add the `/name` GET+POST and `/reboot` handlers**

In `src/config_portal.c`, immediately after the `wifi_post` function (currently ends `src/config_portal.c:226`), add:

```c
#ifdef USE_ZIGBEE
static esp_err_t name_get(httpd_req_t *req) {
    s_idle_ticks = 0;

    char device_id[33] = {0};
    bool has_id = wifi_credentials_load_device_id(device_id, sizeof(device_id));
    char id_esc[33 * 6];
    html_escape_attr(has_id ? device_id : "", id_esc, sizeof(id_esc));

    const size_t body_len = 1024;
    char *body = malloc(body_len);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    snprintf(body, body_len,
        "<!DOCTYPE html><html><head><title>Sensor Name</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
        ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
        "input{width:100%%;padding:10px;margin:10px 0;box-sizing:border-box}"
        "button{background:#4CAF50;color:white;padding:14px;border:none;width:100%%;cursor:pointer;font-size:16px}"
        "a{display:block;text-align:center;margin-top:14px}</style></head>"
        "<body><div class='container'><h2>Sensor Name</h2>"
        "<p>Shown on the display and published to MQTT as <code>label</code> "
        "(used by Node-RED). Max 16 characters.</p>"
        "<form action='/name' method='POST'>"
        "<label>Sensor name:</label>"
        "<input type='text' name='device_id' value='%s' maxlength='16' placeholder='greenhouse01' required>"
        "<button type='submit'>Save &amp; Reboot</button></form>"
        "<a href='/'>Back</a></div></body></html>",
        id_esc);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static esp_err_t name_post(httpd_req_t *req) {
    s_idle_ticks = 0;
    int total = req->content_len;
    if (total <= 0 || total > 512) { httpd_resp_send_500(req); return ESP_FAIL; }
    char *buf = malloc(total + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        got += r;
    }
    buf[total] = '\0';

    char device_id[33] = {0};
    form_field_t fields[] = {
        {"device_id", device_id, sizeof(device_id)},
    };
    bool ok = form_parser_extract(buf, fields, 1);
    free(buf);
    if (!ok || device_id[0] == '\0') { httpd_resp_send_500(req); return ESP_FAIL; }

    // Cap at 16 chars to match the Zigbee LocationDescription limit.
    if (strlen(device_id) > 16) device_id[16] = '\0';

    if (wifi_credentials_save_device_id(device_id) != ESP_OK) {
        httpd_resp_send_500(req); return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<html><body><h1>Saved. Rebooting…</h1></body></html>",
        HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();   // RTC flag already cleared -> boots back into Zigbee
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<html><body><h1>Rebooting…</h1></body></html>",
        HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
#endif /* USE_ZIGBEE */
```

- [ ] **Step 3: Register the new handlers**

In `src/config_portal.c`, in `start_http()`, immediately after the WiFi handlers are registered (the `httpd_register_uri_handler(s_server, &wifi_p);` line, currently `src/config_portal.c:401`), add:

```c
#ifdef USE_ZIGBEE
    httpd_uri_t name_g = {.uri = "/name",   .method = HTTP_GET,  .handler = name_get};
    httpd_uri_t name_p = {.uri = "/name",   .method = HTTP_POST, .handler = name_post};
    httpd_uri_t rb_p   = {.uri = "/reboot", .method = HTTP_POST, .handler = reboot_post};
    httpd_register_uri_handler(s_server, &name_g);
    httpd_register_uri_handler(s_server, &name_p);
    httpd_register_uri_handler(s_server, &rb_p);
#endif
```

> Note: the `/wifi` GET+POST handlers remain registered in the Zigbee build too, but nothing links to them (the Zigbee menu omits the WiFi entry), so they are simply unreachable. Leaving them avoids `#ifdef`-ing out `wifi_get`/`wifi_post` and their statics.

- [ ] **Step 4: Replace the app_main config-mode stub with the real portal**

In `src/main.c`, replace the Task 1 stub body (the `if (s_config_requested) { ... }` block added in Task 1 Step 3) with:

```c
#ifdef USE_ZIGBEE
    // Config-portal mode takes priority over the brownout gate: a deliberate
    // button press should always reach config (radio is off, so power is modest).
    // Clear the flag first so a crash mid-portal returns to normal operation.
    if (s_config_requested) {
        s_config_requested = false;
        ESP_LOGI(TAG, "Entering config portal (button-triggered)");
        if (display_init() == ESP_OK) {
            display_show_portal();
            display_deinit();
        }
        config_portal_run();   // blocks until a save handler restarts, or idle timeout
        esp_restart();         // idle-timeout path: reboot back into Zigbee
    }
#endif
```

- [ ] **Step 5: Build the Zigbee env**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: `SUCCESS`.

- [ ] **Step 6: Build the WiFi env (no regression)**

Run: `pio run`
Expected: `SUCCESS` (the `#else` menu branch and unchanged handlers preserve WiFi behavior).

- [ ] **Step 7: Commit**

```bash
git add src/config_portal.c src/main.c
git commit -m "zigbee: button-triggered config portal (sensor name + calibration)"
```

- [ ] **Step 8: On-device verification (user flashes)**

Flash the Zigbee env. Verify:
1. Press GPIO7 → device reboots, e-paper shows the portal screen, serial logs `AP up: FireBeetle_C6_Prov`.
2. Connect a phone to `FireBeetle_C6_Prov`, open `http://192.168.4.1`. The menu shows **Set Sensor Name / Calibrate / Status / Done–Reboot / Factory Reset** (no WiFi entry).
3. Set a sensor name (e.g. `greenhouse01`) → Save & Reboot → device reboots, rejoins Zigbee, e-paper telemetry screen shows the new name.
4. Press GPIO7 again → Calibrate → dry-in-air / wet-in-water capture with live mV → Save; then Done–Reboot → rejoins Zigbee.
5. Confirm `genBasic.locationDesc` in z2m now reads the new name.

---

## Task 4: zigbee2mqtt converter — surface `label`

Maps the device's LocationDescription into the published MQTT payload so Node-RED can identify the sensor.

**Files:**
- Modify: `z2m/dfr_soil_moisture.js`

- [ ] **Step 1: Add the label converter, expose, and configure**

Replace the contents of `z2m/dfr_soil_moisture.js` with:

```js
// zigbee2mqtt external converter for the DFR-MoistureTracker DIY soil sensor
// (ESP32-C6, esp-zigbee 1.6.x). Exposes:
//   - battery          (Power Configuration cluster 0x0001)
//   - soil_moisture %  (carried on the Relative Humidity cluster 0x0405, since the
//                       SDK's custom Soil Moisture 0x0408 cluster asserts; humidity
//                       and soil moisture share the same 0.01% uint16 wire format)
//   - label            (device-set sensor name, from Basic cluster
//                       LocationDescription 0x0010; published in every payload so
//                       Node-RED can identify the physical sensor)
//
// Install: copy this file into the zigbee2mqtt config dir, reference it under
//   external_converters:
//     - dfr_soil_moisture.js
// (older z2m) or drop it in the `external_converters` folder (newer z2m), then
// restart zigbee2mqtt. The device's modelID "DFR-SoilSensor" matches zigbeeModel,
// so no re-pair is needed — a restart re-applies the definition. After updating,
// trigger a re-interview / "reconfigure" in z2m so the configure() below reads
// locationDesc.

const {battery, numeric} = require('zigbee-herdsman-converters/lib/modernExtend');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;
const ea = exposes.access;

const fzLabel = {
    cluster: 'genBasic',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.data.locationDesc !== undefined) {
            return {label: msg.data.locationDesc};
        }
    },
};

module.exports = [
    {
        zigbeeModel: ['DFR-SoilSensor'],
        model: 'DFR-SoilSensor',
        vendor: 'DFRobot-DIY',
        description: 'ESP32-C6 soil moisture + battery sensor (DIY)',
        extend: [
            battery({
                percentage: true,
                voltage: true,
            }),
            numeric({
                name: 'soil_moisture',
                cluster: 'msRelativeHumidity',          // 0x0405
                attribute: 'measuredValue',
                scale: 100,                              // 0.01% units -> %
                unit: '%',
                precision: 1,
                description: 'Soil moisture',
                access: 'STATE',
                reporting: {min: '10_SECONDS', max: '1_HOUR', change: 50},
            }),
        ],
        fromZigbee: [fzLabel],
        exposes: [
            e.text('label', ea.STATE).withDescription('Device-set sensor name (Node-RED identifier)'),
        ],
        configure: async (device, coordinatorEndpoint, logger) => {
            const ep = device.getEndpoint(1);
            await ep.read('genBasic', ['locationDesc']);
        },
    },
];
```

- [ ] **Step 2: Commit**

```bash
git add z2m/dfr_soil_moisture.js
git commit -m "z2m: surface device sensor name as label payload field"
```

- [ ] **Step 3: On-device / z2m verification (user)**

1. Copy the updated `dfr_soil_moisture.js` into the z2m config dir (replacing the old one) and restart zigbee2mqtt.
2. In z2m, open the device and click **Reconfigure** (gear/▸) so `configure()` runs and reads `locationDesc`.
3. Confirm the device's MQTT payload (z2m → device → state, or the MQTT topic `zigbee2mqtt/<friendly_name>`) now contains `"label": "<sensor name>"`.
4. If `label` does not appear: check z2m logs for converter errors. If the `extend` + top-level `exposes`/`fromZigbee`/`configure` combination is rejected by this z2m version, the fallback is to wrap them in a custom `modernExtend` object inside the `extend` array (`{exposes, fromZigbee, configure, isModernExtend: true}`); report the z2m version and error and I will adjust.

---

## Task 5: Documentation

**Files:**
- Modify: `DEVELOPER_GUIDE.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Document the config portal in the Zigbee build section**

In `DEVELOPER_GUIDE.md`, in the `## Zigbee Build (SP1)` section, replace the "Known limitations (SP1)" bullet about commissioning with an updated pair and add a new subsection just above "Known limitations". Find:

```markdown
- Commissioning is auto-steer-on-boot only; button-driven pairing arrives in SP3.
```

Replace it with:

```markdown
- Pairing is auto-steer-on-boot only; button-driven *pairing* arrives in SP3
  (the GPIO7 button is used for the config portal in SP2 — see below).
```

Then, immediately above the `### Known limitations (SP1)` heading, add:

```markdown
### On-device config (SP2)

A short press of the **GPIO7 button** reboots the device into a SoftAP config
portal (`FireBeetle_C6_Prov`, `http://192.168.4.1`) with **no WiFi-station setup** —
the Zigbee stack is not running in this mode, so there is no radio coexistence
issue. From a phone you can:

- **Set Sensor Name** — persisted to NVS (`device_id`), shown on the e-paper, and
  written to the Basic cluster LocationDescription (0x0010). The z2m converter
  surfaces it as the `label` field in every MQTT payload, so Node-RED can identify
  the physical sensor. Max 16 characters.
- **Calibrate Sensor** — the standard dry/wet capture flow with live mV readout.
- **Done – Reboot** — returns immediately to normal Zigbee operation (otherwise the
  portal exits on a 10-minute idle timeout).

The device reboots back into Zigbee on save/reboot and auto-rejoins with its
persisted network credentials.
```

- [ ] **Step 2: Note the button + label in CLAUDE.md**

In `CLAUDE.md`, in the "### Zigbee build" subsection, immediately after the paragraph ending `(z2m/dfr_soil_moisture.js).`, add:

```markdown
A short press of the GPIO7 button reboots the Zigbee build into the SoftAP config
portal (calibration + sensor name; no WiFi-station setup), then reboots back into
Zigbee. The sensor name is stored in NVS (`device_id`), shown on the e-paper, and
exposed over Zigbee via Basic LocationDescription (0x0010) → surfaced by the z2m
converter as the `label` MQTT field for Node-RED.
```

- [ ] **Step 3: Commit**

```bash
git add DEVELOPER_GUIDE.md CLAUDE.md
git commit -m "docs: document SP2 button config portal + sensor name label"
```

---

## Self-Review notes

- **Spec coverage:** entry mechanism (Task 1), brownout-priority fork (Task 1 Step 3 / Task 3 Step 4), config portal calibration reuse + name page with WiFi fields hidden (Task 3), name persisted to `device_id` (Task 3), name on e-paper (existing telemetry screen uses `device_id_buffer`, set in the SP1 Zigbee path; Task 2 Step 4 sets it before init), name over Zigbee LocationDescription (Task 2), `label` in MQTT payload (Task 4), docs (Task 5), GPIO-wake risk validated first (Task 1 Step 8). All spec sections map to a task.
- **No host tests:** intentional — this firmware has no automated test harness (CLAUDE.md); the prior draft's pure state machine was dropped in the spec revision, so there is no new pure unit to TDD. Validation is build + on-device, matching the project's established practice.
- **Type/name consistency:** `s_config_requested` (flag), `s_config_btn_sem`, `config_btn_isr`, `config_trigger_task`, `setup_config_button` (main.c); `zigbee_reporter_set_location` + `s_location_zcl` (reporter); `/name`, `/reboot`, `name_get`, `name_post`, `reboot_post` (portal); `label` + `fzLabel` (z2m). Consistent across tasks.
