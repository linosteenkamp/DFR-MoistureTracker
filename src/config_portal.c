#include "config_portal.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "CONFIG_PORTAL";
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_should_exit = false;
static int  s_idle_ticks = 0;

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

static esp_err_t root_get(httpd_req_t *req) {
    s_idle_ticks = 0;
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html_menu, HTTPD_RESP_USE_STRLEN);
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

    // (Other handlers registered in Tasks 9–11.)
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
