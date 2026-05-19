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
static bool s_should_exit = false;
static int  s_idle_ticks = 0;
static int  s_pending_dry_mv = -1;
static int  s_pending_wet_mv = -1;

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

static const char *html_wifi_form =
    "<!DOCTYPE html><html><head><title>WiFi Setup</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
    "input{width:100%;padding:10px;margin:10px 0;box-sizing:border-box}"
    "button{background:#4CAF50;color:white;padding:14px;border:none;width:100%;cursor:pointer;font-size:16px}"
    "a{display:block;text-align:center;margin-top:14px}</style></head>"
    "<body><div class='container'><h2>WiFi &amp; Device ID</h2>"
    "<form action='/wifi' method='POST'>"
    "<label>SSID:</label><input type='text' name='ssid' required>"
    "<label>Password:</label><input type='password' name='password' required>"
    "<label>Device ID:</label><input type='text' name='device_id' placeholder='moisture01' required>"
    "<button type='submit'>Save &amp; Restart</button></form>"
    "<a href='/'>Back</a></div></body></html>";

static const char *html_wifi_saved =
    "<!DOCTYPE html><html><body><h1>WiFi saved.</h1>"
    "<p>Device will restart in 2 seconds.</p></body></html>";

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
    "async function cap(k){let r=await fetch('/api/calibrate/'+k,{method:'POST'});let j=await r.json();"
    "document.getElementById(k).textContent='captured: '+j.mv+' mV';}"
    "async function save(){let r=await fetch('/api/calibrate/save',{method:'POST'});"
    "if(r.ok){document.body.innerHTML='<h1>Saved.</h1>';setTimeout(()=>location.href='/',1500);}"
    "else{alert('Capture both DRY and WET first.');}}"
    "</script></body></html>";

static esp_err_t root_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_menu, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_wifi_form, HTTPD_RESP_USE_STRLEN);
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

    if (wifi_credentials_save(ssid, password) != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }
    if (wifi_credentials_save_device_id(device_id) != ESP_OK) { httpd_resp_send_500(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "text/html");
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
    uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000);  // seconds since boot
    bool ok = soil_calibration_save(
        (uint32_t)s_pending_dry_mv, (uint32_t)s_pending_wet_mv, ts);
    if (!ok) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
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
    s_should_exit = false;
    s_idle_ticks = 0;

    esp_err_t err = start_softap();
    if (err != ESP_OK) { stop_server(); return err; }
    err = start_http();
    if (err != ESP_OK) { stop_server(); return err; }

    while (!s_should_exit && s_idle_ticks < PORTAL_TIMEOUT_SEC) {
        vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
        s_idle_ticks++;
    }

    ESP_LOGI(TAG, "Portal exiting (timeout=%d should_exit=%d)",
             s_idle_ticks >= PORTAL_TIMEOUT_SEC, s_should_exit);
    stop_server();
    return ESP_OK;
}
