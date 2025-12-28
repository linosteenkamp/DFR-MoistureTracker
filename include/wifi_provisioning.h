#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief WiFi provisioning interface
 * 
 * Single Responsibility: Handles WiFi provisioning via SoftAP and web interface
 */

/**
 * @brief Start WiFi provisioning mode
 * 
 * Creates a SoftAP and starts HTTP server for credential configuration
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_provisioning_start(void);

/**
 * @brief Check if provisioning is complete
 * @return true if provisioning completed, false otherwise
 */
bool wifi_provisioning_is_complete(void);

/**
 * @brief Stop provisioning and clean up resources
 */
void wifi_provisioning_stop(void);

#endif // WIFI_PROVISIONING_H
