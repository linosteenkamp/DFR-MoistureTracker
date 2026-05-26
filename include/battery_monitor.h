#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#ifndef TEST_HOST
#include "esp_err.h"
#endif

/**
 * @brief Battery monitoring interface
 *
 * Single Responsibility: Monitors battery voltage via ADC
 */

#ifndef TEST_HOST
/**
 * @brief Initialize the battery monitor
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_monitor_init(void);

/**
 * @brief Read current battery voltage
 * @return Battery voltage in volts
 */
float battery_monitor_read_voltage(void);

/**
 * @brief Clean up battery monitor resources
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t battery_monitor_deinit(void);
#endif // TEST_HOST

#endif // BATTERY_MONITOR_H
