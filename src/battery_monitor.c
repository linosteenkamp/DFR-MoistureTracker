#include "battery_monitor.h"
#include "adc_manager.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"

static const char *TAG = "BATTERY";

// FireBeetle 2 C6 Battery is on GPIO 0 -> ADC1 Channel 0
#define BAT_ADC_CHAN          ADC_CHANNEL_0 
#define ADC_ATTEN             ADC_ATTEN_DB_12
#define VOLTAGE_DIVIDER       2.0f    // Hardware divider (1M + 1M ohm resistors)
#define SAMPLE_COUNT          10      // Number of samples to average

static adc_cali_handle_t cali_handle = NULL;
static bool initialized = false;

esp_err_t battery_monitor_init(void) {
    if (initialized) {
        ESP_LOGW(TAG, "Battery monitor already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing battery monitor");

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
    
    esp_err_t err = adc_oneshot_config_channel(adc_handle, BAT_ADC_CHAN, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(err));
        return err;
    }

    // Get or create calibration handle
    err = adc_manager_create_cali(BAT_ADC_CHAN, ADC_ATTEN, &cali_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create calibration: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "Battery monitor initialized on ADC1 Channel %d", BAT_ADC_CHAN);
    
    return ESP_OK;
}

float battery_monitor_read_voltage(void) {
    if (!initialized) {
        ESP_LOGE(TAG, "Battery monitor not initialized");
        return 0.0f;
    }

    // Get shared ADC handle
    adc_oneshot_unit_handle_t adc_handle = adc_manager_get_handle();
    if (!adc_handle) {
        ESP_LOGE(TAG, "ADC handle not available");
        return 0.0f;
    }

    // Take multiple samples and average
    uint32_t value_sum = 0;
    int valid_samples = 0;
    
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        int raw_value = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, BAT_ADC_CHAN, &raw_value);
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

    // Convert to voltage
    int voltage_mV = 0;
    esp_err_t err = adc_cali_raw_to_voltage(cali_handle, avg_raw, &voltage_mV);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to convert ADC value to voltage: %s", esp_err_to_name(err));
        return 0.0f;
    }

    // Apply voltage divider factor
    float battery_voltage = (float)voltage_mV * VOLTAGE_DIVIDER / 1000.0f;

    ESP_LOGI(TAG, "Raw ADC: %d, Pin voltage: %d mV, Battery: %.3f V", 
             avg_raw, voltage_mV, battery_voltage);

    return battery_voltage;
}

esp_err_t battery_monitor_deinit(void) {
    if (!initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing battery monitor");
    
    cali_handle = NULL;
    initialized = false;
    ESP_LOGI(TAG, "Battery monitor deinitialized");
    
    return ESP_OK;
}
