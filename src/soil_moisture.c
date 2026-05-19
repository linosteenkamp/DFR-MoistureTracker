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

#ifndef TEST_HOST
#include "soil_moisture.h"
#include "adc_manager.h"
#include "soil_calibration.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

// Pure percentage math, callable from host tests.
// Linear interpolation between dry (0%) and wet (100%) mV thresholds.
float soil_moisture_calc_percentage(int raw_mv, int dry_mv, int wet_mv) {
    if (raw_mv >= dry_mv) return 0.0f;
    if (raw_mv <= wet_mv) return 100.0f;
    float span = (float)(dry_mv - wet_mv);
    if (span <= 0.0f) return 0.0f;
    float pct = 100.0f * (float)(dry_mv - raw_mv) / span;
    if (pct < 0.0f) return 0.0f;
    if (pct > 100.0f) return 100.0f;
    return pct;
}

#ifndef TEST_HOST

static const char *TAG = "SOIL_MOISTURE";

// ============================================================================
// Configuration Constants
// ============================================================================
// Adjust these values based on your hardware setup and calibration

#define SOIL_ADC_CHAN         ADC_CHANNEL_2    ///< GPIO2 = ADC1_CH2 (AOUT / yellow)
#define ADC_ATTEN             ADC_ATTEN_DB_12  ///< 12dB attenuation for 0-3.1V range
#define SAMPLE_COUNT          10               ///< Number of ADC samples to average (noise reduction)
#define SOIL_PWR_GPIO         GPIO_NUM_3       ///< GPIO3 = sensor VCC (red) — driven HIGH only during read
#define SOIL_WARMUP_MS        150              ///< Settle time after powering sensor before sampling

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

    // Release any deep-sleep hold left on the power pin from the previous wake
    gpio_hold_dis(SOIL_PWR_GPIO);

    // Configure sensor power pin as output, idle LOW (sensor off)
    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << SOIL_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t pwr_err = gpio_config(&pwr_conf);
    if (pwr_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure power GPIO %d: %s", SOIL_PWR_GPIO, esp_err_to_name(pwr_err));
        return pwr_err;
    }
    gpio_set_level(SOIL_PWR_GPIO, 0);

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
    ESP_LOGI(TAG, "Calibration (runtime): Dry=%u mV, Wet=%u mV",
             (unsigned)soil_calibration_get_dry_mv(),
             (unsigned)soil_calibration_get_wet_mv());
    
    return ESP_OK;
}

// ============================================================================
// Voltage Reading
// ============================================================================

// Returns averaged sensor mV, or -1 on hard failure.
static int sample_raw_mv(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return -1;
    }
    adc_oneshot_unit_handle_t adc_handle = adc_manager_get_handle();
    if (!adc_handle) {
        ESP_LOGE(TAG, "ADC handle not available");
        return -1;
    }
    gpio_set_level(SOIL_PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(SOIL_WARMUP_MS));

    uint32_t sum = 0;
    int n = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(adc_handle, SOIL_ADC_CHAN, &raw) == ESP_OK) {
            sum += raw; n++;
        }
    }
    gpio_set_level(SOIL_PWR_GPIO, 0);
    if (n == 0) return -1;

    int mv = 0;
    if (adc_cali_raw_to_voltage(cali_handle, (int)(sum / n), &mv) != ESP_OK) return -1;
    return mv;
}

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
    int mv = sample_raw_mv();
    if (mv < 0) return 0.0f;
    ESP_LOGD(TAG, "Raw mV: %d", mv);
    return (float)mv / 1000.0f;
}

int soil_moisture_read_raw_mv(void) {
    int mv = sample_raw_mv();
    return mv < 0 ? 0 : mv;
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

    // Convert voltage to percentage using pure math function
    float percentage = soil_moisture_calc_percentage(
        voltage_mV,
        (int)soil_calibration_get_dry_mv(),
        (int)soil_calibration_get_wet_mv());

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

#endif // TEST_HOST
