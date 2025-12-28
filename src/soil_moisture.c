/**
 * @file soil_moisture.c
 * @brief DFRobot Capacitive Soil Moisture Sensor interface
 * 
 * This module interfaces with the DFRobot Waterproof Capacitive Soil Moisture Sensor 2.
 * It reads analog voltage from the sensor and converts it to a moisture percentage using
 * calibration values.
 * 
 * Sensor Characteristics:
 * - Operating voltage: 3.3V-5.5V (we use 3.3V)
 * - Output: Analog voltage 0-3V (inversely proportional to moisture)
 * - Technology: Capacitive sensing (no direct soil contact)
 * - Response time: <1 second
 * 
 * Moisture Relationship:
 * - Dry (air): ~2.5-3.0V → 0% moisture
 * - Wet (water): ~1.0-1.5V → 100% moisture
 * - Lower voltage = More moisture (inverse relationship)
 * 
 * Usage:
 * 1. Call soil_moisture_init() after adc_manager_init()
 * 2. Call soil_moisture_read_percentage() for 0-100% reading
 * 3. Call soil_moisture_read_voltage() for raw voltage
 * 
 * Calibration:
 * - Measure sensor in air (dry) → update SENSOR_DRY_MV
 * - Measure sensor in water (wet) → update SENSOR_WET_MV
 * - See SOIL_MOISTURE_SETUP.md for detailed calibration procedure
 * 
 * @author DFRobot Project
 * @date 2025
 */

#include "soil_moisture.h"
#include "adc_manager.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"

static const char *TAG = "SOIL_MOISTURE";

// ============================================================================
// Configuration Constants
// ============================================================================
// Adjust these values based on your hardware setup and calibration

#define SOIL_ADC_CHAN         ADC_CHANNEL_1    ///< GPIO1 = ADC1_CH1 (change if different wiring)
#define ADC_ATTEN             ADC_ATTEN_DB_12  ///< 12dB attenuation for 0-3.1V range
#define SAMPLE_COUNT          10               ///< Number of ADC samples to average (noise reduction)

// ============================================================================
// Calibration Values - CUSTOMIZE THESE FOR YOUR SENSOR
// ============================================================================
// Measure your specific sensor in air (dry) and water (wet) conditions.
// Update these values for accurate moisture percentage readings.
// See SOIL_MOISTURE_SETUP.md for calibration procedure.

#define SENSOR_DRY_MV         2950   ///< Voltage in air (dry) - typically 2500-3000 mV
#define SENSOR_WET_MV         851    ///< Voltage in water (wet) - typically 1000-1500 mV

// Static module state
static adc_cali_handle_t cali_handle = NULL;  ///< ADC calibration handle from adc_manager
static bool initialized = false;              ///< Initialization flag

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize soil moisture sensor
 * 
 * Sets up ADC channel for the sensor and obtains calibration handle from
 * the shared ADC manager. Must be called after adc_manager_init().
 * 
 * Initialization steps:
 * 1. Check if already initialized (idempotent)
 * 2. Get shared ADC handle from manager
 * 3. Configure ADC channel (GPIO, attenuation, bit width)
 * 4. Create/get calibration handle for voltage conversion
 * 5. Log calibration values for verification
 * 
 * @return ESP_OK on success
 * @return ESP_OK if already initialized
 * @return ESP_ERR_INVALID_STATE if ADC manager not initialized
 * @return Error code from ESP-IDF on configuration failure
 * 
 * @note Safe to call multiple times
 * @note Check serial log for "Calibration: Dry=X mV, Wet=Y mV" message
 */

esp_err_t soil_moisture_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Soil moisture sensor already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing soil moisture sensor");

    // Get shared ADC handle
    adc_oneshot_unit_handle_t adc_handle = adc_manager_get_handle();
    if (!adc_handle) {
        ESP_LOGE(TAG, "ADC manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Configure ADC channel
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    esp_err_t err = adc_oneshot_config_channel(adc_handle, SOIL_ADC_CHAN, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(err));
        return err;
    }

    // Get or create calibration handle
    err = adc_manager_create_cali(SOIL_ADC_CHAN, ADC_ATTEN, &cali_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create calibration: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "Soil moisture sensor initialized on ADC1 Channel %d", SOIL_ADC_CHAN);
    ESP_LOGI(TAG, "Calibration: Dry=%d mV, Wet=%d mV", SENSOR_DRY_MV, SENSOR_WET_MV);
    
    return ESP_OK;
}

// ============================================================================
// Voltage Reading
// ============================================================================

/**
 * @brief Read raw sensor voltage
 * 
 * Performs ADC reading and returns the calibrated voltage from the sensor.
 * Takes multiple samples and averages them for noise reduction.
 * 
 * Reading Process:
 * 1. Verify sensor is initialized
 * 2. Take SAMPLE_COUNT (10) ADC readings
 * 3. Average the readings
 * 4. Apply calibration to convert to millivolts
 * 5. Convert to volts and return
 * 
 * @return float Sensor voltage in volts (e.g., 1.85V)
 * @return 0.0 if sensor not initialized or reading failed
 * 
 * @note Higher voltage = drier conditions
 * @note Lower voltage = wetter conditions
 * @note Typical range: 1.0V (wet) to 3.0V (dry)
 */

float soil_moisture_read_voltage(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return 0.0f;
    }

    // Get shared ADC handle
    adc_oneshot_unit_handle_t adc_handle = adc_manager_get_handle();
    if (!adc_handle) {
        ESP_LOGE(TAG, "ADC handle not available");
        return 0.0f;
    }

    // Take multiple samples and average to reduce noise
    uint32_t value_sum = 0;
    int valid_samples = 0;
    
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int raw_value = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, SOIL_ADC_CHAN, &raw_value);
        if (err == ESP_OK) {
            value_sum += raw_value;
            valid_samples++;
        } else {
            ESP_LOGW(TAG, "ADC read failed on sample %d: %s", i, esp_err_to_name(err));
        }
    }

    if (valid_samples == 0) {
        ESP_LOGE(TAG, "All ADC reads failed");
        return 0.0f;
    }

    int avg_raw = value_sum / valid_samples;

    // Convert raw ADC value to voltage using calibration
    int voltage_mV = 0;
    esp_err_t err = adc_cali_raw_to_voltage(cali_handle, avg_raw, &voltage_mV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to convert ADC to voltage: %s", esp_err_to_name(err));
        return 0.0f;
    }

    float voltage = (float)voltage_mV / 1000.0f;
    
    ESP_LOGD(TAG, "Raw ADC: %d, Voltage: %.3f V (%d mV)", avg_raw, voltage, voltage_mV);

    return voltage;
}

// ============================================================================
// Percentage Conversion
// ============================================================================

/**
 * @brief Read soil moisture as percentage
 * 
 * Main function for getting moisture level. Reads sensor voltage and converts
 * to an intuitive 0-100% scale using linear interpolation between calibration points.
 * 
 * Conversion Process:
 * 1. Read raw voltage using soil_moisture_read_voltage()
 * 2. Convert volts to millivolts
 * 3. Apply linear interpolation:
 *    - If voltage >= SENSOR_DRY_MV → 0% (dry)
 *    - If voltage <= SENSOR_WET_MV → 100% (wet)
 *    - Otherwise: percentage = 100 * (DRY - voltage) / (DRY - WET)
 * 4. Clamp result to 0-100% range
 * 
 * Linear Interpolation Formula:
 *   percentage = 100 * (dry_voltage - current_voltage) / (dry_voltage - wet_voltage)
 * 
 * Example:
 *   DRY = 2950mV, WET = 851mV, Current = 1850mV
 *   percentage = 100 * (2950 - 1850) / (2950 - 851) = 52.4%
 * 
 * @return float Moisture percentage (0.0 = dry air, 100.0 = submerged)
 * @return 0.0 if sensor not initialized
 * 
 * @note 0% = Sensor in open air (completely dry)
 * @note 100% = Sensor fully submerged in water
 * @note 30-60% = Typical moist soil range
 * @note Values may exceed 0-100% if sensor readings outside calibration range
 * 
 * Extension:
 * - For non-linear conversion, replace interpolation with lookup table or polynomial
 * - Add temperature compensation if needed
 * - Apply moving average filter for smoother readings
 */

float soil_moisture_read_percentage(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return 0.0f;
    }

    // Read sensor voltage in mV
    float voltage = soil_moisture_read_voltage();
    int voltage_mV = (int)(voltage * 1000.0f);

    // Convert voltage to percentage using linear interpolation
    // Lower voltage = more water (higher moisture)
    // Higher voltage = less water (lower moisture)
    float percentage;
    
    if (voltage_mV >= SENSOR_DRY_MV) {
        // Sensor is dry (in air)
        percentage = 0.0f;
    } else if (voltage_mV <= SENSOR_WET_MV) {
        // Sensor is fully wet
        percentage = 100.0f;
    } else {
        // Linear interpolation between wet and dry
        // Invert the scale: lower voltage = higher percentage
        percentage = 100.0f * (float)(SENSOR_DRY_MV - voltage_mV) / 
                     (float)(SENSOR_DRY_MV - SENSOR_WET_MV);
    }

    // Clamp to valid range
    if (percentage < 0.0f) percentage = 0.0f;
    if (percentage > 100.0f) percentage = 100.0f;

    ESP_LOGI(TAG, "Moisture: %.1f%% (%.3f V, %d mV)", percentage, voltage, voltage_mV);

    return percentage;
}

// ============================================================================
// Cleanup
// ============================================================================

/**
 * @brief Deinitialize soil moisture sensor
 * 
 * Releases sensor resources. In current implementation, calibration handle
 * is managed by adc_manager, so this just clears the local reference.
 * 
 * @return ESP_OK always succeeds
 * 
 * @note Safe to call even if not initialized
 * @note After calling this, sensor must be reinitialized before reading
 * @note ADC manager and calibration handles persist (shared resource)
 */

esp_err_t soil_moisture_deinit(void) {
    if (!initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing soil moisture sensor");

    cali_handle = NULL;
    initialized = false;
    ESP_LOGI(TAG, "Soil moisture sensor deinitialized");
    
    return ESP_OK;
}
