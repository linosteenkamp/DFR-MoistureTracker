#include "wifi_manager.h"
#include "wifi_credentials.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";
static bool wifi_connected = false;

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        wifi_connected = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

esp_err_t wifi_manager_init_sta(void) {
    ESP_LOGI(TAG, "Initializing WiFi in station mode");
    
    // Create default network interface for WiFi station
    esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return err;
    }

    // Register event handlers for WiFi and IP events
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler");
        return err;
    }
    
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return err;
    }

    // Load credentials
    wifi_config_t wifi_config = {0};
    char ssid[33] = {0};
    char password[65] = {0};
    
    if (!wifi_credentials_load(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGE(TAG, "No WiFi credentials found");
        return ESP_FAIL;
    }
    
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode");
        return err;
    }
    
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config");
        return err;
    }
    
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return err;
    }
    
    ESP_LOGI(TAG, "WiFi initialization complete");
    return ESP_OK;
}

bool wifi_manager_is_connected(void) {
    return wifi_connected;
}

bool wifi_manager_wait_connected(int timeout_sec) {
    ESP_LOGI(TAG, "Waiting for WiFi connection (timeout: %d seconds)", timeout_sec);
    
    int elapsed = 0;
    while (!wifi_connected && elapsed < timeout_sec) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed++;
    }
    
    if (wifi_connected) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return true;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return false;
    }
}
