#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief WiFi connection management interface
 * 
 * Single Responsibility: Manages WiFi connection state and operations
 */

/**
 * @brief Initialize WiFi in station mode with stored credentials
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_manager_init_sta(void);

/**
 * @brief Check if WiFi is connected
 * @return true if connected, false otherwise
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Wait for WiFi connection with timeout
 * @param timeout_sec Maximum time to wait in seconds
 * @return true if connected within timeout, false otherwise
 */
bool wifi_manager_wait_connected(int timeout_sec);

#endif // WIFI_MANAGER_H
