#include "battery_monitor.h"
#include "battery_soc.h"
#include <math.h>
#include <stddef.h>

// ============================================================================
// Pure helpers (host-testable)
// ============================================================================

typedef struct { float v; float pct; } soc_point_t;

// 11-point LiPo discharge curve, descending by voltage.
static const soc_point_t SOC_LUT[] = {
    {4.20f, 100.0f},
    {4.05f,  90.0f},
    {3.96f,  80.0f},
    {3.90f,  70.0f},
    {3.85f,  60.0f},
    {3.80f,  50.0f},
    {3.76f,  40.0f},
    {3.73f,  30.0f},
    {3.70f,  20.0f},
    {3.65f,  10.0f},
    {3.20f,   0.0f},
};
static const size_t SOC_LUT_N = sizeof(SOC_LUT) / sizeof(SOC_LUT[0]);

float battery_monitor_v_to_pct(float volts) {
    // Defensive: NaN or negative -> 0%
    if (isnan(volts) || volts <= 0.0f) return 0.0f;
    // Clamp above max
    if (volts >= SOC_LUT[0].v) return SOC_LUT[0].pct;
    // Clamp below min
    if (volts <= SOC_LUT[SOC_LUT_N - 1].v) return SOC_LUT[SOC_LUT_N - 1].pct;
    // Find the bracketing pair (table is descending in voltage)
    for (size_t i = 0; i < SOC_LUT_N - 1; i++) {
        float v_hi = SOC_LUT[i].v;
        float v_lo = SOC_LUT[i + 1].v;
        if (volts <= v_hi && volts >= v_lo) {
            float pct_hi = SOC_LUT[i].pct;
            float pct_lo = SOC_LUT[i + 1].pct;
            float frac = (volts - v_lo) / (v_hi - v_lo);
            return pct_lo + frac * (pct_hi - pct_lo);
        }
    }
    return 0.0f;  // unreachable given the clamps above
}

bool battery_monitor_is_safe(float volts) {
    return volts >= BATTERY_LOW_CUTOFF_V;
}

#ifndef TEST_HOST
#include "adc_manager.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_log.h"
#endif

#ifndef TEST_HOST

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

#endif // TEST_HOST
