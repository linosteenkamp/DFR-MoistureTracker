#include "config_portal.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "wifi_credentials.h"
#include "form_parser.h"
#include <stdlib.h>
#include "soil_calibration.h"
#include "soil_moisture.h"
#include <stdio.h>
#include "esp_timer.h"

static const char *TAG = "CONFIG_PORTAL";
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static int  s_idle_ticks = 0;
static int  s_pending_dry_mv = -1;
static int  s_pending_wet_mv = -1;

// Minimal HTML escape for single-quoted attribute values (handles &, ', <, >).
// out_len should be >= 6x input length + 1 for worst-case all-escape input.
static void html_escape_attr(const char *in, char *out, size_t out_len) {
    if (!out_len) return;
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 6 < out_len; i++) {
        switch (in[i]) {
            case '&':  memcpy(out + j, "&amp;",  5); j += 5; break;
            case '\'': memcpy(out + j, "&#39;",  5); j += 5; break;
            case '<':  memcpy(out + j, "&lt;",   4); j += 4; break;
            case '>':  memcpy(out + j, "&gt;",   4); j += 4; break;
            default:   out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

#define PROV_AP_SSID         "FireBeetle_C6_Prov"
#define PORTAL_TIMEOUT_SEC   600
#define IDLE_TICK_MS         1000

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

static const char *html_wifi_saved =
    "<!DOCTYPE html><html><body><h1>WiFi saved.</h1>"
    "<p>Device will restart in 2 seconds.</p></body></html>";

// Shown after first-boot WiFi save when no calibration has been captured yet.
// Linear flow: WiFi save -> calibrate -> restart (in api_calibrate_save).
static const char *html_wifi_saved_next_step =
    "<!DOCTYPE html><html><head><title>WiFi saved</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".c{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto;text-align:center}"
    "a.btn{display:block;padding:14px;margin:14px 0;background:#4CAF50;color:white;"
    "text-decoration:none;border-radius:4px;font-size:16px}</style></head>"
    "<body><div class='c'><h2>WiFi saved \xE2\x9C\x93</h2>"
    "<p>One more step \xE2\x80\x94 calibrate the sensor so readings are accurate.</p>"
    "<a class='btn' href='/calibrate'>Calibrate Sensor \xE2\x86\x92</a>"
    "</div></body></html>";

static const char *html_calibrate =
    "<!DOCTYPE html><html><head><title>Calibrate</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".c{background:white;padding:30px;border-radius:10px;max-width:480px;margin:auto}"
    ".live{font-size:32px;text-align:center;padding:14px;background:#eef;border-radius:6px;margin:14px 0}"
    "button{background:#4CAF50;color:white;padding:12px;border:none;width:100%;cursor:pointer;font-size:15px;margin:6px 0}"
    ".captured{padding:8px;background:#dfd;border-radius:4px;text-align:center}"
    "a{display:block;text-align:center;margin-top:14px}</style></head>"
    "<body><div class='c'><h2>Calibrate Sensor</h2>"
    "<div class='live' id='live'>… mV</div>"
    "<p>1. Hold sensor in <b>open air</b>, then:</p>"
    "<button onclick='cap(\"dry\")'>Capture DRY</button>"
    "<div class='captured' id='dry'>not captured</div>"
    "<p>2. Submerge sensor to MAX line, then:</p>"
    "<button onclick='cap(\"wet\")'>Capture WET</button>"
    "<div class='captured' id='wet'>not captured</div>"
    "<button onclick='save()'>Save &amp; Restart</button>"
    "<a href='/'>Back</a></div>"
    "<script>"
    "async function poll(){try{let r=await fetch('/api/reading');let j=await r.json();"
    "document.getElementById('live').textContent=j.raw_mv+' mV ('+j.percentage.toFixed(1)+'%)';}catch(e){}}"
    "setInterval(poll,1000);poll();"
    "async function cap(k){let r=await fetch('/api/calibrate/'+k,{method:'POST'});"
    "if(!r.ok){document.getElementById(k).textContent='read failed — check wiring';return;}"
    "let j=await r.json();document.getElementById(k).textContent='captured: '+j.mv+' mV';}"
    "async function save(){let r=await fetch('/api/calibrate/save',{method:'POST'});"
    "if(!r.ok){alert('Capture both DRY and WET first.');return;}"
    "let j=await r.json();"
    "document.body.innerHTML=j.restart?'<h1>Saved. Restarting\xE2\x80\xA6</h1>':'<h1>Saved.</h1>';"
    "if(!j.restart)setTimeout(()=>location.href='/',1500);}"
    "</script></body></html>";

static const char *html_reset_confirm =
    "<!DOCTYPE html><html><head><title>Factory Reset</title>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".c{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto;text-align:center}"
    "button{background:#d9534f;color:white;padding:14px;border:none;width:100%;cursor:pointer;font-size:16px;margin:8px 0}"
    "a{display:block;margin-top:14px}</style></head>"
    "<body><div class='c'><h2>Factory Reset</h2>"
    "<p>This wipes WiFi credentials and calibration. Device will restart and ask for setup again.</p>"
    "<form action='/factory-reset' method='POST'>"
    "<button type='submit'>Yes, wipe everything</button></form>"
    "<a href='/'>Cancel</a></div></body></html>";

static esp_err_t root_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_menu, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_get(httpd_req_t *req) {
    s_idle_ticks = 0;

    // Pre-populate from NVS so a user changing one field doesn't retype the others.
    // Password is intentionally never echoed back — placeholder indicates whether
    // one is saved; an empty submission preserves it.
    char ssid[33] = {0};
    char password[65] = {0};
    char device_id[33] = {0};
    bool has_creds      = wifi_credentials_load(ssid, sizeof(ssid), password, sizeof(password));
    bool has_device_id  = wifi_credentials_load_device_id(device_id, sizeof(device_id));

    char ssid_esc[33 * 6];
    char device_id_esc[33 * 6];
    html_escape_attr(has_creds     ? ssid      : "", ssid_esc,      sizeof(ssid_esc));
    html_escape_attr(has_device_id ? device_id : "", device_id_esc, sizeof(device_id_esc));

    const char *pw_placeholder = has_creds ? "(saved \xE2\x80\x94 leave blank to keep)" : "WiFi password";
    const char *pw_required    = has_creds ? "" : " required";

    // Heap-allocate body — httpd task stack is ~4 KB, and a 1.5 KB buffer
    // here plus the escape buffers above blew it (observed stack overflow).
    const size_t body_len = 1536;
    char *body = malloc(body_len);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    snprintf(body, body_len,
        "<!DOCTYPE html><html><head><title>WiFi Setup</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
        ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
        "input{width:100%%;padding:10px;margin:10px 0;box-sizing:border-box}"
        "button{background:#4CAF50;color:white;padding:14px;border:none;width:100%%;cursor:pointer;font-size:16px}"
        "a{display:block;text-align:center;margin-top:14px}</style></head>"
        "<body><div class='container'><h2>WiFi &amp; Device ID</h2>"
        "<form action='/wifi' method='POST'>"
        "<label>SSID:</label><input type='text' name='ssid' value='%s' required>"
        "<label>Password:</label><input type='password' name='password' placeholder='%s'%s>"
        "<label>Device ID:</label><input type='text' name='device_id' value='%s' placeholder='moisture01' required>"
        "<button type='submit'>Save &amp; Restart</button></form>"
        "<a href='/'>Back</a></div></body></html>",
        ssid_esc, pw_placeholder, pw_required, device_id_esc);

    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static esp_err_t wifi_post(httpd_req_t *req) {
    s_idle_ticks = 0;
    int total = req->content_len;
    if (total <= 0 || total > 1024) { httpd_resp_send_500(req); return ESP_FAIL; }
    char *buf = malloc(total + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        got += r;
    }
    buf[total] = '\0';

    char ssid[33] = {0}, password[65] = {0}, device_id[33] = {0};
    form_field_t fields[] = {
        {"ssid",      ssid,      sizeof(ssid)},
        {"password",  password,  sizeof(password)},
        {"device_id", device_id, sizeof(device_id)},
    };
    bool ok = form_parser_extract(buf, fields, 3);
    free(buf);
    if (!ok) { httpd_resp_send_500(req); return ESP_FAIL; }

    // Empty password field = keep the existing one (UX: user is only changing other fields).
    const char *password_to_save = password;
    char existing_ssid[33] = {0};
    char existing_password[65] = {0};
    if (password[0] == '\0' &&
        wifi_credentials_load(existing_ssid, sizeof(existing_ssid),
                              existing_password, sizeof(existing_password))) {
        password_to_save = existing_password;
    }

    if (wifi_credentials_save(ssid, password_to_save) != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (wifi_credentials_save_device_id(device_id) != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }

    // First-boot flow: if no calibration has been captured yet, don't restart
    // — send the user to /calibrate first. api_calibrate_save will restart
    // after the first calibration lands. Return visits (cal_ts != 0) keep
    // the original instant-restart behaviour.
    httpd_resp_set_type(req, "text/html");
    if (soil_calibration_get_cal_ts() == 0) {
        return httpd_resp_send(req, html_wifi_saved_next_step, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send(req, html_wifi_saved, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t calibrate_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_calibrate, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_reading_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    int raw = soil_moisture_read_raw_mv();
    uint32_t dry = soil_calibration_get_dry_mv();
    uint32_t wet = soil_calibration_get_wet_mv();
    float pct = soil_moisture_calc_percentage(raw, (int)dry, (int)wet);

    char body[160];
    snprintf(body, sizeof(body),
        "{\"raw_mv\":%d,\"percentage\":%.1f,\"dry_mv\":%u,\"wet_mv\":%u}",
        raw, pct, (unsigned)dry, (unsigned)wet);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_capture(httpd_req_t *req, int *target) {
    s_idle_ticks = 0;
    int mv = soil_moisture_read_raw_mv();
    // 0 mV from soil_moisture_read_raw_mv signals a hard failure
    // (sensor not initialised or all ADC reads failed). Don't persist that
    // as a real capture — return an error so the UI can prompt the user
    // to check wiring instead of silently saving garbage calibration.
    if (mv <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "sensor read failed (check wiring)");
        return ESP_FAIL;
    }
    *target = mv;
    char body[40];
    snprintf(body, sizeof(body), "{\"mv\":%d}", mv);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_calibrate_dry(httpd_req_t *req) {
    return api_capture(req, &s_pending_dry_mv);
}
static esp_err_t api_calibrate_wet(httpd_req_t *req) {
    return api_capture(req, &s_pending_wet_mv);
}

static esp_err_t api_calibrate_save(httpd_req_t *req) {
    s_idle_ticks = 0;
    if (s_pending_dry_mv < 0 || s_pending_wet_mv < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "capture dry and wet first");
        return ESP_FAIL;
    }
    // First-boot detection: if no prior calibration, restart after this save
    // so the device joins WiFi and starts publishing telemetry. On re-cal
    // trips we stay in the portal so the user can keep using it.
    bool was_first_cal = (soil_calibration_get_cal_ts() == 0);
    // esp_timer_get_time() / 1e6 is always > 0 after init; clamp to 1 so
    // cal_ts can never collide with the "never calibrated" sentinel.
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000);
    if (ts == 0) ts = 1;
    bool ok = soil_calibration_save(
        (uint32_t)s_pending_dry_mv, (uint32_t)s_pending_wet_mv, ts);
    if (!ok) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req,
        was_first_cal ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true}",
        HTTPD_RESP_USE_STRLEN);
    if (was_first_cal) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    int raw = soil_moisture_read_raw_mv();
    uint32_t dry = soil_calibration_get_dry_mv();
    uint32_t wet = soil_calibration_get_wet_mv();
    uint32_t ts  = soil_calibration_get_cal_ts();
    float pct = soil_moisture_calc_percentage(raw, (int)dry, (int)wet);

    // Heap-allocate body for the same reason as wifi_get — httpd task
    // stack is small and putting near-1 KB buffers on it is unsafe.
    const size_t body_len = 768;
    char *body = malloc(body_len);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    snprintf(body, body_len,
        "<!DOCTYPE html><html><head><title>Status</title>"
        "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
        ".c{background:white;padding:30px;border-radius:10px;max-width:480px;margin:auto}"
        "table{width:100%%}td{padding:6px 0}td.k{color:#666;width:40%%}"
        "a{display:block;text-align:center;margin-top:14px}</style></head>"
        "<body><div class='c'><h2>Status</h2><table>"
        "<tr><td class='k'>DRY mV</td><td>%u</td></tr>"
        "<tr><td class='k'>WET mV</td><td>%u</td></tr>"
        "<tr><td class='k'>Last cal (s)</td><td>%u</td></tr>"
        "<tr><td class='k'>Live mV</td><td>%d</td></tr>"
        "<tr><td class='k'>Live %%</td><td>%.1f</td></tr>"
        "</table><a href='/'>Back</a></div></body></html>",
        (unsigned)dry, (unsigned)wet, (unsigned)ts, raw, pct);
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static esp_err_t factory_reset_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_reset_confirm, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t factory_reset_post(httpd_req_t *req) {
    s_idle_ticks = 0;
    wifi_credentials_clear();
    soil_calibration_clear();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<html><body><h1>Wiped. Restarting…</h1></body></html>",
        HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_softap(void) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = PROV_AP_SSID,
            .ssid_len = strlen(PROV_AP_SSID),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "AP up: %s — open http://192.168.4.1", PROV_AP_SSID);
    return ESP_OK;
}

static esp_err_t start_http(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t wifi_g = {.uri = "/wifi", .method = HTTP_GET,  .handler = wifi_get,  .user_ctx = NULL};
    httpd_uri_t wifi_p = {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post, .user_ctx = NULL};
    httpd_register_uri_handler(s_server, &wifi_g);
    httpd_register_uri_handler(s_server, &wifi_p);

    httpd_uri_t cal_g  = {.uri = "/calibrate",         .method = HTTP_GET,  .handler = calibrate_get};
    httpd_uri_t cal_r  = {.uri = "/api/reading",       .method = HTTP_GET,  .handler = api_reading_get};
    httpd_uri_t cal_d  = {.uri = "/api/calibrate/dry", .method = HTTP_POST, .handler = api_calibrate_dry};
    httpd_uri_t cal_w  = {.uri = "/api/calibrate/wet", .method = HTTP_POST, .handler = api_calibrate_wet};
    httpd_uri_t cal_s  = {.uri = "/api/calibrate/save",.method = HTTP_POST, .handler = api_calibrate_save};
    httpd_register_uri_handler(s_server, &cal_g);
    httpd_register_uri_handler(s_server, &cal_r);
    httpd_register_uri_handler(s_server, &cal_d);
    httpd_register_uri_handler(s_server, &cal_w);
    httpd_register_uri_handler(s_server, &cal_s);

    httpd_uri_t st_g  = {.uri = "/status",        .method = HTTP_GET,  .handler = status_get};
    httpd_uri_t fr_g  = {.uri = "/factory-reset", .method = HTTP_GET,  .handler = factory_reset_get};
    httpd_uri_t fr_p  = {.uri = "/factory-reset", .method = HTTP_POST, .handler = factory_reset_post};
    httpd_register_uri_handler(s_server, &st_g);
    httpd_register_uri_handler(s_server, &fr_g);
    httpd_register_uri_handler(s_server, &fr_p);

    return ESP_OK;
}

static void stop_server(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
}

esp_err_t config_portal_run(void) {
    ESP_LOGI(TAG, "Starting config portal");
    s_idle_ticks = 0;

    esp_err_t err = start_softap();
    if (err != ESP_OK) { stop_server(); return err; }
    err = start_http();
    if (err != ESP_OK) { stop_server(); return err; }

    // Only exit path is the idle timeout — handlers that change state
    // (WiFi save, factory reset) call esp_restart() and never return.
    while (s_idle_ticks < PORTAL_TIMEOUT_SEC) {
        vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
        s_idle_ticks++;
    }

    ESP_LOGI(TAG, "Portal exiting (idle timeout)");
    stop_server();
    return ESP_OK;
}
