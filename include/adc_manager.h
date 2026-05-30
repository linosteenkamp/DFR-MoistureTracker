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

/**
 * @brief Tear down and rebuild the shared ADC unit and all calibration schemes.
 *
 * The ADC1 analog/pad state for the soil channel does not survive ESP32-C6 light
 * sleep (reads rail to >4000 mV); only a complete unit recreate recovers it.
 * After calling this, every sensor must re-establish its channel + calibration
 * via its *_reconfigure() helper, since the old handles are now invalid.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t adc_manager_reinit(void);

#endif // ADC_MANAGER_H
