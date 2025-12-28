#include "wifi_credentials.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WIFI_CREDS";

// NVS keys for WiFi credentials
#define NVS_NAMESPACE         "wifi_config"
#define NVS_KEY_SSID          "ssid"
#define NVS_KEY_PASSWORD      "password"
#define NVS_KEY_PROVISIONED   "provisioned"
#define NVS_KEY_DEVICE_ID     "device_id"

bool wifi_credentials_is_provisioned(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, device not provisioned");
        return false;
    }
    
    uint8_t prov_flag = 0;
    err = nvs_get_u8(nvs_handle, NVS_KEY_PROVISIONED, &prov_flag);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK && prov_flag == 1) {
        ESP_LOGI(TAG, "Device is provisioned");
        return true;
    }
    
    ESP_LOGW(TAG, "Device not provisioned");
    return false;
}

bool wifi_credentials_load(char *ssid, size_t ssid_len, char *password, size_t pass_len) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS");
        return false;
    }
    
    size_t required_size = ssid_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &required_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SSID from NVS");
        nvs_close(nvs_handle);
        return false;
    }
    
    required_size = pass_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password, &required_size);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read password from NVS");
        return false;
    }
    
    ESP_LOGI(TAG, "Loaded WiFi credentials from NVS: SSID=%s", ssid);
    return true;
}

esp_err_t wifi_credentials_save(const char *ssid, const char *password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write SSID to NVS");
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write password to NVS");
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_u8(nvs_handle, NVS_KEY_PROVISIONED, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set provisioned flag");
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved successfully");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS changes");
    }
    
    return err;
}

esp_err_t wifi_credentials_save_device_id(const char *device_id) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing device ID");
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_DEVICE_ID, device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write device ID to NVS");
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Device ID saved successfully: %s", device_id);
    } else {
        ESP_LOGE(TAG, "Failed to commit device ID");
    }
    
    return err;
}

bool wifi_credentials_load_device_id(char *device_id, size_t device_id_len) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading device ID");
        return false;
    }
    
    size_t required_size = device_id_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_DEVICE_ID, device_id, &required_size);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No device ID found in NVS");
        return false;
    }
    
    ESP_LOGI(TAG, "Loaded device ID from NVS: %s", device_id);
    return true;
}

esp_err_t wifi_credentials_clear(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for clearing");
        return err;
    }
    
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials cleared");
    } else {
        ESP_LOGE(TAG, "Failed to clear credentials");
    }
    
    return err;
}
