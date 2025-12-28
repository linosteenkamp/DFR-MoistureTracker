#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief WiFi credentials storage interface
 * 
 * Single Responsibility: Manages WiFi credentials persistence in NVS
 */

/**
 * @brief Check if device has been provisioned with WiFi credentials
 * @return true if provisioned, false otherwise
 */
bool wifi_credentials_is_provisioned(void);

/**
 * @brief Load WiFi credentials from NVS
 * @param ssid Buffer to store SSID (min 33 bytes)
 * @param ssid_len Size of SSID buffer
 * @param password Buffer to store password (min 65 bytes)
 * @param pass_len Size of password buffer
 * @return true if credentials loaded successfully, false otherwise
 */
bool wifi_credentials_load(char *ssid, size_t ssid_len, char *password, size_t pass_len);

/**
 * @brief Save WiFi credentials to NVS
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_credentials_save(const char *ssid, const char *password);

/**
 * @brief Save device ID to NVS
 * @param device_id Device identifier (e.g., "sensor02")
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_credentials_save_device_id(const char *device_id);

/**
 * @brief Load device ID from NVS
 * @param device_id Buffer to store device ID (min 33 bytes)
 * @param device_id_len Size of device ID buffer
 * @return true if device ID loaded successfully, false otherwise
 */
bool wifi_credentials_load_device_id(char *device_id, size_t device_id_len);

/**
 * @brief Clear all WiFi credentials from NVS
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_credentials_clear(void);

#endif // WIFI_CREDENTIALS_H
