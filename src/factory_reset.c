/**
 * @file factory_reset.c
 * @brief Factory reset button implementation
 * 
 * Monitors GPIO button with debouncing and long press detection.
 * Clears all WiFi credentials from NVS and restarts device on long press.
 */

#include "factory_reset.h"
#include "wifi_credentials.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

static const char *TAG = "FACTORY_RESET";

// Configuration
#define FACTORY_RESET_GPIO       GPIO_NUM_20         // GPIO pin for reset button (change if needed)
#define LONG_PRESS_DURATION_MS   5000                // Hold for 5 seconds
#define DEBOUNCE_TIME_MS         50                  // Debounce period
#define BUTTON_PRESSED           0                   // Active low (button to GND)

// State tracking
static uint32_t button_press_start_ms = 0;
static bool button_was_pressed = false;
static bool reset_triggered = false;

/**
 * @brief Get current time in milliseconds
 */
static uint32_t get_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**
 * @brief Read current button state with debouncing
 */
static bool is_button_pressed(void) {
    static uint32_t last_change_time = 0;
    static bool last_stable_state = false;
    
    bool current_state = (gpio_get_level(FACTORY_RESET_GPIO) == BUTTON_PRESSED);
    uint32_t now = get_time_ms();
    
    // If state changed, update timestamp
    if (current_state != last_stable_state) {
        if (now - last_change_time >= DEBOUNCE_TIME_MS) {
            last_stable_state = current_state;
            last_change_time = now;
        }
    }
    
    return last_stable_state;
}

esp_err_t factory_reset_init(void) {
    ESP_LOGI(TAG, "Initializing factory reset button on GPIO %d", FACTORY_RESET_GPIO);
    
    // Configure GPIO as input with pull-up
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO");
        return err;
    }
    
    ESP_LOGI(TAG, "Factory reset button initialized (long press = %d seconds)", 
             LONG_PRESS_DURATION_MS / 1000);
    
    return ESP_OK;
}

void factory_reset_check(void) {
    // Don't check if already triggered
    if (reset_triggered) {
        return;
    }
    
    bool currently_pressed = is_button_pressed();
    uint32_t now = get_time_ms();
    
    // Button just pressed
    if (currently_pressed && !button_was_pressed) {
        button_press_start_ms = now;
        button_was_pressed = true;
        ESP_LOGI(TAG, "Button pressed - hold for %d seconds to factory reset", 
                 LONG_PRESS_DURATION_MS / 1000);
    }
    // Button still held
    else if (currently_pressed && button_was_pressed) {
        uint32_t press_duration = now - button_press_start_ms;
        
        // Long press detected
        if (press_duration >= LONG_PRESS_DURATION_MS) {
            reset_triggered = true;
            ESP_LOGW(TAG, "Factory reset triggered!");
            ESP_LOGW(TAG, "Clearing WiFi credentials...");
            
            // Clear credentials
            wifi_credentials_clear();
            
            ESP_LOGW(TAG, "Restarting in 2 seconds...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        // Progress indicator every second
        else if (press_duration % 1000 < 100) {
            ESP_LOGI(TAG, "Hold for %d more seconds...", 
                     (LONG_PRESS_DURATION_MS - press_duration) / 1000);
        }
    }
    // Button released
    else if (!currently_pressed && button_was_pressed) {
        uint32_t press_duration = now - button_press_start_ms;
        button_was_pressed = false;
        
        if (press_duration < LONG_PRESS_DURATION_MS) {
            ESP_LOGI(TAG, "Button released (held for %d ms, need %d ms)", 
                     press_duration, LONG_PRESS_DURATION_MS);
        }
    }
}
