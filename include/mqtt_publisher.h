#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief MQTT publishing interface
 * 
 * Single Responsibility: Manages MQTT connection and publishes telemetry data
 */

/**
 * @brief MQTT configuration structure
 */
typedef struct {
    const char *broker_uri;
    const char *username;
    const char *password;
    const char *base_topic;
    int keepalive_sec;
} mqtt_config_t;

/**
 * @brief Initialize and start MQTT client
 * @param config MQTT configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publisher_init(const mqtt_config_t *config);

/**
 * @brief Check if MQTT client is connected
 * @return true if connected, false otherwise
 */
bool mqtt_publisher_is_connected(void);

/**
 * @brief Publish telemetry data
 * @param battery_voltage Current battery voltage
 * @param soil_moisture Soil moisture percentage (0-100)
 * @param device_name Device identifier
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_publisher_publish_telemetry(float battery_voltage, float soil_moisture, const char *device_name);

#endif // MQTT_PUBLISHER_H
