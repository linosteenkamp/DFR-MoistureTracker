#ifndef SOIL_MOISTURE_H
#define SOIL_MOISTURE_H

#include "esp_err.h"

/**
 * @brief Soil moisture sensor interface
 * 
 * Single Responsibility: Monitors soil moisture via capacitive sensor
 * 
 * DFRobot Waterproof Capacitive Soil Moisture Sensor 2:
 * - Analog output (voltage proportional to moisture)
 * - Operating voltage: 3.3V-5.5V
 * - Output voltage: 0-3V
 * - Response time: <1s
 */

/**
 * @brief Initialize the soil moisture sensor
 * 
 * Configures the ADC channel for reading the sensor.
 * This function creates and configures persistent ADC handles
 * that will be reused for subsequent readings.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t soil_moisture_init(void);

/**
 * @brief Read current soil moisture level as percentage
 * 
 * Reads the analog sensor value, applies calibration,
 * and converts to a percentage (0-100%).
 * 
 * - 0% = Sensor in air (dry)
 * - 100% = Sensor fully submerged in water (wet)
 * 
 * @return Soil moisture percentage (0.0 - 100.0)
 */
float soil_moisture_read_percentage(void);

/**
 * @brief Read raw sensor voltage
 * 
 * Returns the actual voltage reading from the sensor
 * without any conversion to percentage.
 * 
 * @return Sensor voltage in volts
 */
float soil_moisture_read_voltage(void);

/**
 * @brief Clean up soil moisture sensor resources
 * 
 * Releases ADC handles and calibration schemes.
 * Should be called before shutdown or reconfiguration.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t soil_moisture_deinit(void);

#endif // SOIL_MOISTURE_H
