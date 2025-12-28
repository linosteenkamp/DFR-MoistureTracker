/**
 * @file main.c
 * @brief DFR-MoistureTracker - Main application entry point
 * 
 * This is the main application file for the DFR-MoistureTracker environmental monitoring system.
 * It orchestrates all subsystems including WiFi connectivity, sensor reading, and MQTT telemetry.
 * 
 * Architecture:
 * - Follows SOLID principles with clear separation of concerns
 * - Each subsystem is encapsulated in its own module
 * - Main coordinates initialization and periodic telemetry publishing
 * 
 * Power Management:
 * - Uses deep sleep between readings for battery efficiency
 * - Wakes every DEEP_SLEEP_INTERVAL_SEC (1 hour by default)
 * - Reads sensors, publishes to MQTT, then sleeps again
 * 
 * Flow:
 * 1. Initialize system infrastructure (NVS, network, ADC)
 * 2. Setup WiFi (provisioning if needed)
 * 3. Connect to MQTT broker
 * 4. Read sensors and publish telemetry
 * 5. Enter deep sleep for configured interval
 * 6. Wake and repeat from step 2 (WiFi reconnect)
 * 
 * @author DFRobot Project
 * @date 2025
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// Module interfaces following SOLID principles
#include "wifi_credentials.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"
#include "adc_manager.h"
#include "battery_monitor.h"
#include "soil_moisture.h"
#include "mqtt_publisher.h"
#include "factory_reset.h"
#include "mqtt_credentials.h"  // MQTT broker credentials (not in git)

static const char *TAG = "MAIN";

// Static buffer for MQTT topic (persists across function calls)
static char mqtt_topic_buffer[128] = {0};
static char device_id_buffer[33] = {0};

// ============================================================================
// Application Configuration
// ============================================================================
// These values can be customized for your deployment
// MQTT credentials are defined in include/mqtt_credentials.h (not committed to git)
// MQTT_BROKER_URI, MQTT_USERNAME, MQTT_PASSWORD, MQTT_TOPIC_PREFIX, MQTT_KEEPALIVE_SEC

#define MQTT_BASE_TOPIC      MQTT_TOPIC_PREFIX           ///< Base topic (deprecated, uses prefix + device_id)x + device_id)
#define MQTT_KEEPALIVE_SEC   10                          ///< MQTT keepalive interval in seconds
#define DEFAULT_DEVICE_ID    "sensor02"                  ///< Fallback device ID if not provisioned
#define WIFI_TIMEOUT_SEC     30                          ///< WiFi connection timeout before retry
#define MQTT_WAIT_MS         3000                        ///< Time to wait for MQTT connection (milliseconds)
#define PUBLISH_WAIT_MS      2000                        ///< Time to wait after publishing before sleep (milliseconds)

// Deep Sleep Configuration
#define DEEP_SLEEP_INTERVAL_SEC  3600                    ///< Deep sleep duration in seconds (3600 = 1 hour)
#define uS_TO_S_FACTOR          1000000ULL               ///< Conversion factor for microseconds to seconds

// ============================================================================
// System Initialization
// ============================================================================

/**
 * @brief Initialize system infrastructure
 * 
 * Initializes core ESP32 system components required for operation:
 * - NVS (Non-Volatile Storage) for persistent data
 * - TCP/IP network stack
 * - Event loop for asynchronous events
 * - Shared ADC manager for sensors
 * - Factory reset button monitoring
 * - Battery and soil moisture sensors
 * 
 * Single Responsibility: Initialize core system components only
 * 
 * @return ESP_OK on success, error code otherwise
 * 
 * @note This function must be called before any other initialization
 * @note ADC manager must initialize before battery/soil moisture sensors
 */
static esp_err_t init_system(void) {
    ESP_LOGI(TAG, "Initializing system infrastructure");
    
    // Initialize NVS
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS");
        return ret;
    }
    ESP_LOGI(TAG, "NVS initialized");

    // Initialize TCP/IP network interface
    ESP_LOGI(TAG, "Initializing TCP/IP stack...");
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TCP/IP stack");
        return ret;
    }
    ESP_LOGI(TAG, "TCP/IP stack initialized");

    // Create default event loop
    ESP_LOGI(TAG, "Creating event loop...");
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create event loop");
        return ret;
    }
    ESP_LOGI(TAG, "Event loop created");
    
    // Initialize shared ADC manager
    ESP_LOGI(TAG, "Initializing ADC manager...");
    ret = adc_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC manager");
        return ret;
    }
    ESP_LOGI(TAG, "ADC manager initialized");
    
    // Initialize factory reset button
    ESP_LOGI(TAG, "Initializing factory reset button...");
    ret = factory_reset_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize factory reset button");
        return ret;
    }
    ESP_LOGI(TAG, "Factory reset button initialized");
    
    // Initialize battery monitor
    ESP_LOGI(TAG, "Initializing battery monitor...");
    ret = battery_monitor_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize battery monitor, continuing anyway");
    } else {
        ESP_LOGI(TAG, "Battery monitor initialized");
    }
    
    // Initialize soil moisture sensor
    ESP_LOGI(TAG, "Initializing soil moisture sensor...");
    ret = soil_moisture_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize soil moisture sensor, continuing anyway");
        // Don't fail - continue without soil moisture sensor
    } else {
        ESP_LOGI(TAG, "Soil moisture sensor initialized");
    }
    
    return ESP_OK;
}

// ============================================================================
// WiFi Provisioning
// ============================================================================

/**
 * @brief Handle WiFi provisioning workflow
 * 
 * Manages the complete provisioning process:
 * 1. Starts provisioning mode (creates SoftAP)
 * 2. Waits for user to connect and configure WiFi
 * 3. Restarts device after successful provisioning
 * 
 * Single Responsibility: Manage the provisioning state machine
 * 
 * @note This function blocks until provisioning is complete
 * @note Device automatically restarts after provisioning
 * @note Will not return - device restarts in this function
 */
static void handle_provisioning(void) {
    ESP_LOGI(TAG, "Starting provisioning mode");
    
    if (wifi_provisioning_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning");
        return;
    }
    
    // Wait for provisioning to complete
    ESP_LOGI(TAG, "Waiting for user to configure WiFi...");
    while (!wifi_provisioning_is_complete()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "Provisioning complete, restarting...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

// ============================================================================
// WiFi Connection
// ============================================================================

/**
 * @brief Setup WiFi connection
 * 
 * Establishes WiFi connectivity using stored or provisioned credentials:
 * - Checks if device is already provisioned
 * - If not provisioned, enters provisioning mode
 * - If provisioned, connects to stored WiFi network
 * - On connection failure, clears credentials and restarts
 * 
 * Single Responsibility: Establish WiFi connectivity using stored credentials
 * 
 * @return ESP_OK on successful connection
 * @return ESP_FAIL if not provisioned or connection failed
 * 
 * @note Will restart device if connection fails after timeout
 * @note Will not return if provisioning is triggered (device restarts)
 */
static esp_err_t setup_wifi(void) {
    ESP_LOGI(TAG, "Setting up WiFi connection");
    
    // Check if device is provisioned
    if (!wifi_credentials_is_provisioned()) {
        ESP_LOGI(TAG, "Device not provisioned");
        handle_provisioning();
        // Will restart after provisioning, so this line won't be reached
        return ESP_FAIL;
    }
    
    // Initialize WiFi with stored credentials
    esp_err_t err = wifi_manager_init_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return err;
    }
    
    // Wait for connection
    if (!wifi_manager_wait_connected(WIFI_TIMEOUT_SEC)) {
        ESP_LOGE(TAG, "WiFi connection failed, clearing credentials and restarting");
        wifi_credentials_clear();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "WiFi connected successfully");
    return ESP_OK;
}

// ============================================================================
// MQTT Connection
// ============================================================================

/**
 * @brief Initialize MQTT client
 * 
 * Configures and establishes connection to MQTT broker:
 * - Loads device ID from NVS or uses default
 * - Constructs MQTT topic: zigbee2mqtt/{device_id}
 * - Initializes MQTT client with broker credentials
 * - Starts MQTT connection
 * 
 * Single Responsibility: Configure and start MQTT publisher
 * 
 * @return ESP_OK on successful initialization
 * @return ESP_FAIL if initialization failed
 * 
 * @note Actual connection happens asynchronously
 * @note Use mqtt_publisher_is_connected() to check connection status
 */
static esp_err_t setup_mqtt(void) {
    ESP_LOGI(TAG, "Setting up MQTT connection");
    
    // Load device ID from NVS or use default
    if (!wifi_credentials_load_device_id(device_id_buffer, sizeof(device_id_buffer))) {
        ESP_LOGW(TAG, "No device ID found, using default: %s", DEFAULT_DEVICE_ID);
        strncpy(device_id_buffer, DEFAULT_DEVICE_ID, sizeof(device_id_buffer) - 1);
        device_id_buffer[sizeof(device_id_buffer) - 1] = '\0';
    }
    
    // Construct full MQTT topic in static buffer
    snprintf(mqtt_topic_buffer, sizeof(mqtt_topic_buffer), "%s%s", MQTT_TOPIC_PREFIX, device_id_buffer);
    
    ESP_LOGI(TAG, "Using MQTT topic: %s", mqtt_topic_buffer);
    
    mqtt_config_t config;
    memset(&config, 0, sizeof(config));
    config.broker_uri = MQTT_BROKER_URI;
    config.username = MQTT_USERNAME;
    config.password = MQTT_PASSWORD;
    config.base_topic = mqtt_topic_buffer;  // Use static buffer
    config.keepalive_sec = MQTT_KEEPALIVE_SEC;
    
    esp_err_t err = mqtt_publisher_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MQTT");
        return err;
    }
    
    ESP_LOGI(TAG, "MQTT initialized");
    return ESP_OK;
}

// ============================================================================
// Deep Sleep Management
// ============================================================================

/**
 * @brief Configure and enter deep sleep mode
 * 
 * Configures the ESP32 to wake after the specified interval and enters
 * deep sleep mode. In deep sleep:
 * - CPU is powered off
 * - WiFi is powered off
 * - Most RAM is powered off
 * - Only RTC memory and RTC peripherals remain active
 * - Current draw: ~10µA (vs ~85mA when active)
 * 
 * On wake:
 * - Device performs a full reboot
 * - app_main() is called again
 * - All initialization repeats
 * - RTC memory persists (could store data if needed)
 * 
 * @param seconds Sleep duration in seconds
 * 
 * @note This function does not return - device goes to sleep
 * @note Device will appear to restart when it wakes
 * @note Battery life improvement: ~50x longer with 1 hour intervals
 */
static void enter_deep_sleep(uint32_t seconds) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Entering deep sleep for %lu seconds (%lu minutes)", 
             seconds, seconds / 60);
    ESP_LOGI(TAG, "Device will wake and publish again at next interval");
    ESP_LOGI(TAG, "========================================");
    
    // Configure wake timer
    esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
    
    // Optional: Print wake time for debugging
    int64_t now_us = esp_timer_get_time();
    int64_t wake_us = now_us + (seconds * uS_TO_S_FACTOR);
    ESP_LOGI(TAG, "Current time: %lld µs", now_us);
    ESP_LOGI(TAG, "Wake time: %lld µs", wake_us);
    
    // Flush logs before sleeping
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Enter deep sleep - does not return
    esp_deep_sleep_start();
}

// ============================================================================
// Telemetry Publishing
// ============================================================================

/**
 * @brief Publish single telemetry reading
 * 
 * Performs one-time telemetry reading and publishing, then prepares for deep sleep:
 * 1. Waits for MQTT connection (with timeout)
 * 2. Reads battery voltage
 * 3. Reads soil moisture percentage
 * 4. Publishes telemetry data to MQTT
 * 5. Waits briefly for publish to complete
 * 
 * Single Responsibility: One-time telemetry collection and publishing
 * 
 * @return ESP_OK if telemetry published successfully
 * @return ESP_FAIL if MQTT not connected or publish failed
 * 
 * @note Called once per wake cycle before deep sleep
 * @note Does not loop - publishes once and returns
 * @note MQTT connection happens asynchronously, so we wait briefly
 */
static esp_err_t publish_telemetry_once(void) {
    ESP_LOGI(TAG, "Waiting for MQTT connection...");
    
    // Wait for MQTT to connect (up to MQTT_WAIT_MS)
    int wait_count = 0;
    int max_wait = MQTT_WAIT_MS / 100;  // Check every 100ms
    
    while (!mqtt_publisher_is_connected() && wait_count < max_wait) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }
    
    if (!mqtt_publisher_is_connected()) {
        ESP_LOGW(TAG, "MQTT connection timeout - will retry on next wake");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "MQTT connected, reading sensors...");
    
    // Read battery voltage
    float voltage = battery_monitor_read_voltage();
    
    // Read soil moisture
    float soil_moisture = soil_moisture_read_percentage();
    
    // Publish telemetry
    ESP_LOGI(TAG, "Publishing telemetry: Battery=%.2fV, Moisture=%.1f%%", 
             voltage, soil_moisture);
    
    esp_err_t err = mqtt_publisher_publish_telemetry(voltage, soil_moisture, device_id_buffer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to publish telemetry");
        return ESP_FAIL;
    }
    
    // Wait a bit for message to be sent
    ESP_LOGI(TAG, "Waiting for publish to complete...");
    vTaskDelay(pdMS_TO_TICKS(PUBLISH_WAIT_MS));
    
    ESP_LOGI(TAG, "Telemetry published successfully");
    return ESP_OK;
}

// ============================================================================
// Legacy Telemetry Loop (Not Used with Deep Sleep)
// ============================================================================

/**
 * @brief Main telemetry publishing loop (DEPRECATED - Using deep sleep instead)
 * 
 * This function is no longer used with deep sleep enabled.
 * Left for reference or if switching back to continuous operation.
 * 
 * Original behavior:
 * - Infinite loop checking factory reset and publishing telemetry
 * - Published every TELEMETRY_INTERVAL_MS
 * 
 * @deprecated Use publish_telemetry_once() + deep sleep instead
 */
/**
 * @deprecated Use publish_telemetry_once() + deep sleep instead
 */
static void telemetry_loop(void) {
    ESP_LOGI(TAG, "Starting legacy telemetry loop (not used with deep sleep)");
    
    while (1) {
        // Check for factory reset button
        factory_reset_check();
        
        // Check if both WiFi and MQTT are connected
        if (wifi_manager_is_connected() && mqtt_publisher_is_connected()) {
            // Read battery voltage
            float voltage = battery_monitor_read_voltage();
            
            // Read soil moisture (will return 0 if not initialized)
            float soil_moisture = soil_moisture_read_percentage();
            
            // Publish telemetry using device ID
            esp_err_t err = mqtt_publisher_publish_telemetry(voltage, soil_moisture, device_id_buffer);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to publish telemetry");
            }
        } else {
            ESP_LOGW(TAG, "Not connected, skipping telemetry");
        }
        
        // Wait for next interval (deprecated - using deep sleep instead)
        vTaskDelay(pdMS_TO_TICKS(30000));  // 30 seconds
    }
}

// ============================================================================
// Application Entry Point
// ============================================================================

/**
 * @brief Application entry point
 * 
 * Main application entry called by ESP-IDF after bootloader.
 * 
 * With Deep Sleep Mode:
 * This function is called on every wake from deep sleep, so it runs
 * the complete initialization sequence each time:
 * 
 * 1. Initialize system infrastructure (NVS, network, ADC, sensors)
 * 2. Connect to WiFi (credentials persist in NVS)
 * 3. Connect to MQTT broker
 * 4. Publish single telemetry reading
 * 5. Enter deep sleep for DEEP_SLEEP_INTERVAL_SEC
 * 6. Wake and repeat from step 1
 * 
 * Coordinates all subsystems following Dependency Inversion Principle:
 * - Depends on abstractions (module interfaces) not implementations
 * - Each module has a single responsibility
 * - Open for extension, closed for modification
 * 
 * Power Efficiency:
 * - Active time: ~5 seconds per hour
 * - Sleep time: 3595 seconds per hour
 * - Average current: ~1.7mA (vs ~85mA continuous)
 * - Battery life: ~50x improvement (days → months)
 * 
 * @note Function executes once per wake cycle, then device sleeps
 * @note Device performs full reboot on wake from deep sleep
 * @note WiFi credentials and device ID persist across sleep
 * 
 * Extension Guide:
 * - Add new sensors: Initialize in init_system(), read in publish_telemetry_once()
 * - Change sleep interval: Modify DEEP_SLEEP_INTERVAL_SEC
 * - Add RTC memory: Use RTC_DATA_ATTR for data persistence across sleep
 * - Disable deep sleep: Call telemetry_loop() instead of publish + sleep
 */
void app_main(void) {
    ESP_LOGI(TAG, "=== DFR-MoistureTracker Starting ===");
    ESP_LOGI(TAG, "Wake from deep sleep - initializing...");
    
    // Print wake reason for debugging
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    switch (wake_cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            ESP_LOGI(TAG, "Wake cause: Timer");
            break;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            ESP_LOGI(TAG, "Wake cause: Power on reset or first boot");
            break;
        default:
            ESP_LOGI(TAG, "Wake cause: %d", wake_cause);
            break;
    }
    
    // Step 1: Initialize system infrastructure
    if (init_system() != ESP_OK) {
        ESP_LOGE(TAG, "System initialization failed, entering sleep anyway");
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;  // Never reached
    }
    
    // Step 2: Setup WiFi (handles provisioning if needed)
    if (setup_wifi() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi setup failed, entering sleep");
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;  // Never reached
    }
    
    // Step 3: Setup MQTT
    if (setup_mqtt() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT setup failed, entering sleep");
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;  // Never reached
    }
    
    ESP_LOGI(TAG, "=== Initialization Complete ===");
    
    // Step 4: Publish telemetry once
    esp_err_t err = publish_telemetry_once();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Telemetry publish failed, but continuing to sleep");
    }
    
    // Step 5: Enter deep sleep
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
    
    // This line is never reached - device enters deep sleep
}
