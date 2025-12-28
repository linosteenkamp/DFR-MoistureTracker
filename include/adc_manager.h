#ifndef ADC_MANAGER_H
#define ADC_MANAGER_H

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

/**
 * @brief Shared ADC manager
 * 
 * Manages ADC1 unit handle shared between battery monitor and soil moisture sensor
 */

/**
 * @brief Initialize shared ADC manager
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t adc_manager_init(void);

/**
 * @brief Get ADC1 unit handle
 * @return ADC unit handle or NULL if not initialized
 */
adc_oneshot_unit_handle_t adc_manager_get_handle(void);

/**
 * @brief Get calibration handle for a specific channel
 * @param channel ADC channel
 * @param atten Attenuation level
 * @return Calibration handle or NULL if not available
 */
adc_cali_handle_t adc_manager_get_cali_handle(adc_channel_t channel, adc_atten_t atten);

/**
 * @brief Create or get calibration handle for channel
 * @param channel ADC channel
 * @param atten Attenuation level
 * @param cali_handle Output calibration handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t adc_manager_create_cali(adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *cali_handle);

#endif // ADC_MANAGER_H
