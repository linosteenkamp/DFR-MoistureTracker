/**
 * @file adc_manager.c
 * @brief Shared ADC resource manager for multiple sensors
 * 
 * This module manages a single ADC unit (ADC1) that is shared between multiple
 * analog sensors (battery monitor and soil moisture sensor). It prevents resource
 * conflicts by providing a centralized ADC handle and calibration management.
 * 
 * Key Features:
 * - Single ADC unit handle shared across all sensors
 * - Multiple calibration handles (one per channel/attenuation combination)
 * - Automatic calibration handle reuse
 * - Thread-safe initialization
 * 
 * Usage Pattern:
 * 1. Call adc_manager_init() once at startup
 * 2. Each sensor calls adc_manager_create_cali() to get calibration handle
 * 3. Each sensor calls adc_manager_get_handle() to get ADC unit handle
 * 4. Sensors configure their own channels and perform readings
 * 
 * @note Only supports ADC1 currently
 * @note Maximum 4 calibration handles (one per sensor typically)
 * 
 * @author DFRobot Project
 * @date 2025
 */

#include "adc_manager.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "ADC_MGR";

#define ADC_UNIT              ADC_UNIT_1      ///< Fixed to ADC1 for all sensors
#define MAX_CALI_HANDLES      4               ///< Maximum calibration handles (adjust if more sensors needed)

// Shared ADC unit handle - single instance for all sensors
static adc_oneshot_unit_handle_t adc_handle = NULL;
static bool initialized = false;

/**
 * @brief Calibration handle entry
 * 
 * Stores calibration handles for different channel/attenuation combinations.
 * Allows reuse of calibration handles when multiple sensors share parameters.
 */
typedef struct {
    adc_channel_t channel;        ///< ADC channel (0-4 for ADC1)
    adc_atten_t atten;           ///< Attenuation level (0, 2.5, 6, 11 dB)
    adc_cali_handle_t handle;    ///< ESP-IDF calibration handle
    bool in_use;                 ///< Slot occupied flag
} cali_entry_t;

// Calibration handles storage array
static cali_entry_t cali_handles[MAX_CALI_HANDLES] = {0};

/**
 * @brief Initialize the shared ADC manager
 * 
 * Creates a single ADC1 unit handle that will be shared by all sensors.
 * Must be called before any sensor initialization.
 * 
 * @return ESP_OK on success
 * @return ESP_OK if already initialized (idempotent)
 * @return Error code if ADC unit creation fails
 * 
 * @note Safe to call multiple times - will return immediately if already initialized
 * @note Must be called before battery_monitor_init() and soil_moisture_init()
 */

esp_err_t adc_manager_init(void) {
    if (initialized) {
        ESP_LOGD(TAG, "ADC manager already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing shared ADC manager");

    // Create ADC unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT,
    };
    
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC unit: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "ADC manager initialized");
    
    return ESP_OK;
}

/**
 * @brief Get the shared ADC1 unit handle
 * 
 * Returns the ADC unit handle for sensors to use for channel configuration
 * and ADC readings.
 * 
 * @return adc_oneshot_unit_handle_t ADC unit handle
 * @return NULL if not initialized
 * 
 * @note Check for NULL before using
 * @note All sensors share this same handle
 */

adc_oneshot_unit_handle_t adc_manager_get_handle(void) {
    return adc_handle;
}

/**
 * @brief Get existing calibration handle for channel/attenuation pair
 * 
 * Searches for an existing calibration handle matching the specified
 * channel and attenuation combination.
 * 
 * @param channel ADC channel (ADC_CHANNEL_0 to ADC_CHANNEL_4)
 * @param atten Attenuation level (ADC_ATTEN_DB_0, _2_5, _6, _12)
 * 
 * @return adc_cali_handle_t Existing calibration handle
 * @return NULL if no matching handle found
 * 
 * @note Used internally to avoid creating duplicate calibration handles
 */

adc_cali_handle_t adc_manager_get_cali_handle(adc_channel_t channel, adc_atten_t atten) {
    for (int i = 0; i < MAX_CALI_HANDLES; i++) {
        if (cali_handles[i].in_use && 
            cali_handles[i].channel == channel && 
            cali_handles[i].atten == atten) {
            return cali_handles[i].handle;
        }
    }
    return NULL;
}

/**
 * @brief Create or retrieve calibration handle for sensor
 * 
 * Creates a new calibration handle or returns existing one if already created
 * for the same channel/attenuation combination. This allows multiple sensors
 * to share calibration if they use the same configuration.
 * 
 * Calibration handles are cached to avoid redundant creation and save memory.
 * 
 * @param channel ADC channel for the sensor (ADC_CHANNEL_0 to ADC_CHANNEL_4)
 * @param atten Attenuation level (ADC_ATTEN_DB_0, _2_5, _6, or _12)
 * @param[out] cali_handle Pointer to store the calibration handle
 * 
 * @return ESP_OK on success
 * @return ESP_ERR_INVALID_ARG if cali_handle is NULL
 * @return ESP_ERR_NO_MEM if no free calibration slots available
 * @return Error code from ESP-IDF if calibration creation fails
 * 
 * @note Maximum MAX_CALI_HANDLES (4) different calibration handles can exist
 * @note Reuses existing handle if channel+attenuation already calibrated
 * @note Call this after configuring ADC channel
 * 
 * Extension: Increase MAX_CALI_HANDLES if more sensors with different configs needed
 */

esp_err_t adc_manager_create_cali(adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *cali_handle) {
    if (!cali_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check if already exists
    adc_cali_handle_t existing = adc_manager_get_cali_handle(channel, atten);
    if (existing) {
        *cali_handle = existing;
        return ESP_OK;
    }

    // Find free slot
    int free_slot = -1;
    for (int i = 0; i < MAX_CALI_HANDLES; i++) {
        if (!cali_handles[i].in_use) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        ESP_LOGE(TAG, "No free calibration handle slots");
        return ESP_ERR_NO_MEM;
    }

    // Create calibration scheme
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_config, cali_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create calibration: %s", esp_err_to_name(err));
        return err;
    }

    // Store in slot
    cali_handles[free_slot].channel = channel;
    cali_handles[free_slot].atten = atten;
    cali_handles[free_slot].handle = *cali_handle;
    cali_handles[free_slot].in_use = true;

    ESP_LOGI(TAG, "Created calibration for channel %d, atten %d", channel, atten);
    
    return ESP_OK;
}
