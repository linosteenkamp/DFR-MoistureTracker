/**
 * @file factory_reset.h
 * @brief Factory reset button handler interface
 * 
 * Single Responsibility: GPIO button monitoring for factory reset
 * - Detects long press (5 seconds) on configured GPIO
 * - Clears WiFi credentials and restarts device
 */

#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

#include "esp_err.h"

/**
 * @brief Initialize factory reset button
 * 
 * Configures GPIO with internal pull-up for button input.
 * Button should connect GPIO to GND when pressed.
 * 
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t factory_reset_init(void);

/**
 * @brief Check if factory reset button is being pressed
 * 
 * Call this periodically (e.g., every 100ms) to monitor button state.
 * If button is held for LONG_PRESS_DURATION_MS, triggers factory reset.
 * 
 * This function is non-blocking and handles debouncing internally.
 */
void factory_reset_check(void);

#endif // FACTORY_RESET_H
