#ifndef SOIL_MOISTURE_H
#define SOIL_MOISTURE_H

#ifndef TEST_HOST
#include "esp_err.h"
#endif

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

/**
 * @brief Pure percentage math from raw ADC mV and calibration mV.
 *
 * Linear interpolation, clamped to [0, 100]. No hardware access.
 * Exposed for unit testing and for direct callers that want the math
 * without triggering a physical read.
 */
float soil_moisture_calc_percentage(int raw_mv, int dry_mv, int wet_mv);

/**
 * @brief Read averaged raw sensor value in millivolts.
 *
 * Like soil_moisture_read_voltage() but returns the integer mV from
 * the same 10-sample average. Used by the calibration capture
 * endpoints in config_portal.
 *
 * @return mV (0 if sensor not initialized or all reads fail)
 */
int soil_moisture_read_raw_mv(void);

#endif // SOIL_MOISTURE_H
