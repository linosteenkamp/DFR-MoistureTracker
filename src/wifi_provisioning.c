#include "wifi_provisioning.h"
#include "wifi_credentials.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_PROV";
static httpd_handle_t server = NULL;
static bool provisioning_complete = false;

#define PROV_AP_SSID  "FireBeetle_C6_Prov"

// HTML form for WiFi configuration
static const char *html_form = 
    "<!DOCTYPE html><html><head><title>WiFi Setup</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;margin:40px;background:#f0f0f0}"
    ".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:auto}"
    "input{width:100%;padding:10px;margin:10px 0;box-sizing:border-box}"
    "button{background:#4CAF50;color:white;padding:14px;border:none;width:100%;cursor:pointer;font-size:16px}"
    "button:hover{background:#45a049}</style></head>"
    "<body><div class='container'><h2>FireBeetle C6 WiFi Setup</h2>"
    "<form action='/save' method='POST'>"
    "<label>WiFi SSID:</label><input type='text' name='ssid' required><br>"
    "<label>Password:</label><input type='password' name='password' required><br>"
    "<label>Device ID:</label><input type='text' name='device_id' placeholder='sensor02' required><br>"
    "<button type='submit'>Connect</button></form></div></body></html>";

static const char *html_success = 
    "<!DOCTYPE html><html><head><title>Success</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:Arial;text-align:center;margin-top:50px}</style></head>"
    "<body><h1>WiFi Configured!</h1><p>Device will restart and connect to your network.</p></body></html>";

// HTTP GET handler for root
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_form, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// URL decode helper
static void url_decode(char *str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] == '+') {
            str[i] = ' ';
        }
        // TODO: Handle %XX encoding if needed
    }
}

// Parse URL-encoded form data into ssid, password and device_id
static bool copy_form_field(const char *start, size_t key_len, char *dst, size_t dst_len)
{
    start += key_len;
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= dst_len) return false;
    strncpy(dst, start, len);
    dst[len] = '\0';
    return true;
}

static bool parse_form_fields(const char *buf,
                              char *ssid, size_t ssid_len,
                              char *password, size_t pass_len,
                              char *device_id, size_t device_id_len)
{
    const char *ssid_start = strstr(buf, "ssid=");
    const char *pass_start = strstr(buf, "password=");
    const char *device_id_start = strstr(buf, "device_id=");
    if (!ssid_start || !pass_start || !device_id_start) {
        return false;
    }

    if (!copy_form_field(ssid_start, 5, ssid, ssid_len)) return false;
    if (!copy_form_field(pass_start, 9, password, pass_len)) return false;
    if (!copy_form_field(device_id_start, 10, device_id, device_id_len)) return false;

    url_decode(ssid);
    url_decode(password);
    url_decode(device_id);
    return true;
}

// HTTP POST handler for /save
static esp_err_t save_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    char device_id[33] = {0};

    if (!parse_form_fields(buf, ssid, sizeof(ssid), password, sizeof(password), device_id, sizeof(device_id))) {
        free(buf);
        ESP_LOGE(TAG, "Failed to parse form fields");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    free(buf);

    ESP_LOGI(TAG, "Received credentials - SSID: %s, Device ID: %s", ssid, device_id);

    // Save WiFi credentials to NVS
    if (wifi_credentials_save(ssid, password) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Save device ID to NVS
    if (wifi_credentials_save_device_id(device_id) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_success, HTTPD_RESP_USE_STRLEN);

    // Set provisioning complete flag and restart
    provisioning_complete = true;
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// Start HTTP server
static esp_err_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);
    
    httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save_uri);
    
    ESP_LOGI(TAG, "HTTP server started successfully");
    return ESP_OK;
}

esp_err_t wifi_provisioning_start(void) {
    ESP_LOGI(TAG, "Starting WiFi provisioning");
    
    // Create SoftAP network interface
    esp_netif_create_default_wifi_ap();
    
    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return err;
    }
    
    // Configure SoftAP
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = PROV_AP_SSID,
            .ssid_len = strlen(PROV_AP_SSID),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode");
        return err;
    }
    
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config");
        return err;
    }
    
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return err;
    }
    
    ESP_LOGI(TAG, "WiFi AP started: %s", PROV_AP_SSID);
    ESP_LOGI(TAG, "Connect to this network and go to http://192.168.4.1");
    
    // Start HTTP server
    return start_webserver();
}

bool wifi_provisioning_is_complete(void) {
    return provisioning_complete;
}

void wifi_provisioning_stop(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi provisioning stopped");
}
