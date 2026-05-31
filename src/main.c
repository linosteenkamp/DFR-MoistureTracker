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
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

// Module interfaces following SOLID principles
#include "wifi_credentials.h"
#include "wifi_manager.h"
#include "config_portal.h"
#include "adc_manager.h"
#include "battery_monitor.h"
#include "battery_soc.h"
#include "soil_moisture.h"
#include "soil_calibration.h"
#include "display.h"
#include "mqtt_publisher.h"
#include "mqtt_credentials.h"  // MQTT broker credentials (not in git)
#ifdef USE_ZIGBEE
#include "zigbee_reporter.h"
#include "ota_client.h"
#endif

static const char *TAG = "MAIN";

// Static buffer for MQTT topic (persists across function calls)
static char mqtt_topic_buffer[128] = {0};
static char device_id_buffer[33] = {0};

// Cached OCV captured at the very top of app_main(), reused by publish_telemetry_once().
static float g_cached_battery_v = 0.0f;

// Persists across deep sleep: latches when the low-battery warning has been drawn,
// so we don't burn ~30 mJ refreshing the e-paper every hour while the cell is starved.
RTC_DATA_ATTR static bool s_low_battery_shown = false;

// Set by the GPIO7 trigger task before esp_restart(); read once early in
// app_main() to enter config-portal mode. Must be RTC_NOINIT (not RTC_DATA):
// esp_restart() is a full SW reset, and startup re-initialises .rtc.data from
// flash — so an initialised RTC_DATA var would be wiped back to its initializer.
// .rtc.noinit is never touched by startup, so it survives esp_restart (and deep
// sleep); only a cold power-on randomises it, hence the magic guard.
#ifdef USE_ZIGBEE
#define CONFIG_REQUEST_MAGIC 0xC04F1601u
RTC_NOINIT_ATTR static uint32_t s_config_request_magic;
#endif

// ============================================================================
// Application Configuration
// ============================================================================
// These values can be customized for your deployment
// MQTT credentials are defined in include/mqtt_credentials.h (not committed to git)
// MQTT_BROKER_URI, MQTT_USERNAME, MQTT_PASSWORD, MQTT_TOPIC_PREFIX, MQTT_KEEPALIVE_SEC

#define MQTT_KEEPALIVE_SEC   10                          ///< MQTT keepalive interval in seconds
#define DEFAULT_DEVICE_ID    "moisture01"                ///< Fallback device ID if not provisioned
#define WIFI_TIMEOUT_SEC     30                          ///< WiFi connection timeout before retry
#define MQTT_WAIT_MS         3000                        ///< Time to wait for MQTT connection (milliseconds)
#define PUBLISH_WAIT_MS      2000                        ///< Time to wait after publishing before sleep (milliseconds)

// Deep Sleep Configuration
#define DEEP_SLEEP_INTERVAL_SEC  3600                    ///< Deep sleep duration in seconds (3600 = 1 hour)
#define uS_TO_S_FACTOR          1000000ULL               ///< Conversion factor for microseconds to seconds

#ifdef USE_ZIGBEE
#define ZIGBEE_REPORT_INTERVAL_SEC  900                  ///< Zigbee managed-sleep report interval (seconds, 15 min)
#endif

#ifdef DISABLE_DEEP_SLEEP
#define TEST_PUBLISH_INTERVAL_MS 5000                    ///< Test-mode re-publish cadence (WiFi path only)
#endif

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
    
    // Initialize soil calibration (loads NVS values or falls back to defaults)
    ESP_LOGI(TAG, "Initializing soil calibration...");
    soil_calibration_init();
    ESP_LOGI(TAG, "Soil calibration loaded");
    
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

    // Initialize WiFi with stored credentials
    esp_err_t err = wifi_manager_init_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return err;
    }

    // Wait for connection — on transient failure (router rebooting, rain
    // attenuating 2.4 GHz, AP overloaded) we MUST NOT wipe credentials. Just
    // stop the radio and sleep; next wake retries with the same credentials.
    if (!wifi_manager_wait_connected(WIFI_TIMEOUT_SEC)) {
        ESP_LOGW(TAG, "WiFi connection failed — will retry on next wake");
        wifi_manager_stop();
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

    // Clean radio/network teardown so we don't sleep with a half-open MQTT
    // session or with the WiFi modem still powered. Both helpers are no-ops
    // if their subsystems were never started.
    mqtt_publisher_stop();
    wifi_manager_stop();

    // Isolate the analog input pins (battery on GPIO 0, soil AOUT on GPIO 2)
    // so the digital pads don't leak through pull resistors in sleep.
    rtc_gpio_isolate(GPIO_NUM_0);
    rtc_gpio_isolate(GPIO_NUM_2);

    // Configure wake timer
    esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);

    // Hold the soil-sensor power pin LOW across deep sleep so the sensor stays off.
    // On C6, per-pin hold (gpio_hold_en) persists through deep sleep on its own —
    // the chip uses SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP, so no global enable is needed.
    gpio_hold_en(GPIO_NUM_3);

    // GPIO7 = config-portal wake button (LP-capable, momentary push-to-GND, internal pull-up).
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_7),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);
    esp_deep_sleep_enable_gpio_wakeup(BIT(GPIO_NUM_7), ESP_GPIO_WAKEUP_GPIO_LOW);

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
    
    // Battery voltage was captured at the top of app_main() before WiFi powered up,
    // so it reflects open-circuit voltage rather than the sagging-under-load value.
    float voltage = g_cached_battery_v;
    
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

    // Refresh the e-paper with the values we just published.
    display_telemetry_t dt = {
        .device_id     = device_id_buffer,
        .moisture_pct  = soil_moisture,
        .raw_mv        = soil_moisture_read_raw_mv(),
        .battery_v     = voltage,
        .battery_pct   = display_battery_v_to_pct(voltage),
        .wifi_rssi_dbm = wifi_manager_get_rssi(),
    };
    if (display_init() == ESP_OK) {
        display_show_telemetry(&dt);
        display_deinit();
    }

    return ESP_OK;
}

// ============================================================================
// Portal Mode
// ============================================================================

static void run_portal_then_sleep(void) {
    ESP_LOGI(TAG, "Entering config portal");
    if (display_init() == ESP_OK) {
        display_show_portal();
        display_deinit();
    }
    config_portal_run();   // blocks until save or timeout
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
}

// ============================================================================
// Zigbee Sensor Sampling Callback
// ============================================================================

#ifdef USE_ZIGBEE
// --- GPIO7 config-portal entry (Zigbee build) ---------------------------------
// The Zigbee build runs managed light sleep (CPU mostly halted between radio
// events), so the deep-sleep GPIO-wake path never runs. GPIO7 is armed as a
// LOW-LEVEL light-sleep wake source with a LOW-LEVEL interrupt. The trigger must
// be level- (not edge-) sensitive: the press's falling edge happens while the
// digital GPIO logic is gated in light sleep, so on wake the pin is already held
// low and no edge is ever seen — only a level interrupt fires. The ISR disables
// the interrupt (a held-low level int would otherwise re-fire forever and starve
// the trigger task) and signals s_config_btn_sem; config_trigger_task debounces,
// latches the RTC flag, and restarts into config mode. esp_restart() is illegal
// in ISR context, hence the task hop.
static SemaphoreHandle_t s_config_btn_sem = NULL;

static void IRAM_ATTR config_btn_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(GPIO_NUM_7);   // level int: stop the storm (re-armed on bounce)
    BaseType_t hp_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_config_btn_sem, &hp_woken);
    portYIELD_FROM_ISR(hp_woken);
}

static void config_trigger_task(void *pv)
{
    (void)pv;
    for (;;) {
        if (xSemaphoreTake(s_config_btn_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // Debounce: require the line to still be low ~40 ms after the wake.
        vTaskDelay(pdMS_TO_TICKS(40));
        if (gpio_get_level(GPIO_NUM_7) != 0) {
            gpio_intr_enable(GPIO_NUM_7);   // bounce/noise — re-arm and wait again
            continue;
        }
        ESP_LOGI(TAG, "GPIO7 pressed — rebooting into config mode");
        s_config_request_magic = CONFIG_REQUEST_MAGIC;
        vTaskDelay(pdMS_TO_TICKS(50));   // let the log flush
        esp_restart();
    }
}

static void setup_config_button(void)
{
    s_config_btn_sem = xSemaphoreCreateBinary();
    if (s_config_btn_sem == NULL) {
        ESP_LOGW(TAG, "config button sem alloc failed — button disabled");
        return;
    }

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << GPIO_NUM_7),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_LOW_LEVEL,   // matches the low-level light-sleep wake
    };
    gpio_config(&btn);

    // Wake the CPU out of managed light sleep when GPIO7 goes low.
    gpio_wakeup_enable(GPIO_NUM_7, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        // INVALID_STATE just means the service was already installed elsewhere.
        ESP_LOGW(TAG, "gpio isr service install: %s", esp_err_to_name(isr_err));
    }
    gpio_isr_handler_add(GPIO_NUM_7, config_btn_isr, NULL);

    xTaskCreate(config_trigger_task, "cfg_btn", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "GPIO7 config button armed (low-level light-sleep wake + ISR)");
}
#endif /* USE_ZIGBEE */

#ifdef USE_ZIGBEE
/* --- Periodic report task ----------------------------------------------------
 * The Zigbee reporter fires a cheap "tick" in the stack main-loop context on the
 * report schedule (zb_report_tick). Doing the work there would stall keep-alives
 * and frame handling — the soil read alone blocks ~150 ms warming the sensor, and
 * the SSD1680 full refresh ~2 s. So the tick only signals this task, which:
 *   1. samples every sensor exactly ONCE — a single soil power-up yields both the
 *      raw mV and the %, so the display and the Zigbee report can't disagree;
 *   2. pushes the values over Zigbee via the locked zigbee_reporter_report() path;
 *   3. refreshes the e-paper with that same sample.
 */
static SemaphoreHandle_t s_report_sem = NULL;

static void zb_report_tick(void)
{
    /* Zigbee stack task context (scheduler alarm) — keep it cheap: just wake the
     * report task. Not an ISR, so the plain give is correct. */
    if (s_report_sem) {
        xSemaphoreGive(s_report_sem);
    }
}

static void zb_report_task(void *pv)
{
    (void)pv;
    for (;;) {
        if (xSemaphoreTake(s_report_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // The soil ADC channel's analog state does not survive C6 light sleep
        // (post-sleep reads rail to >4000 mV → 0%), and re-priming the channel
        // alone doesn't recover it. Fully rebuild the shared ADC unit each cycle
        // and re-establish both sensors on the fresh handle before sampling.
        if (adc_manager_reinit() == ESP_OK) {
            soil_moisture_reconfigure();
            battery_monitor_reconfigure();
        } else {
            ESP_LOGE(TAG, "ADC reinit failed — readings may be invalid this cycle");
        }

        // One soil power-up: derive both raw mV and % from the same sample, so
        // the reported value and the displayed value are guaranteed consistent.
        int   raw_mv      = soil_moisture_read_raw_mv();
        float soil_pct    = soil_moisture_calc_percentage(
                                raw_mv,
                                (int)soil_calibration_get_dry_mv(),
                                (int)soil_calibration_get_wet_mv());
        float battery_v   = battery_monitor_read_voltage();
        float battery_pct = battery_monitor_v_to_pct(battery_v);

        // Push to Zigbee (takes the Zigbee lock — we are not the stack task).
        zigbee_reporter_report(soil_pct, battery_v, battery_pct);

        // After the first good report on a freshly-OTA'd image, confirm it so
        // the bootloader keeps the new slot; otherwise it auto-reverts on reboot.
        static bool s_first_report_done = false;
        if (!s_first_report_done) {
            s_first_report_done = true;
            ota_client_mark_valid();
        }

        // Refresh the e-paper with the SAME sample.
        display_telemetry_t dt = {
            .device_id     = device_id_buffer,
            .moisture_pct  = soil_pct,
            .raw_mv        = raw_mv,
            .battery_v     = battery_v,
            .battery_pct   = (int)(battery_pct + 0.5f),
            .wifi_rssi_dbm = 0,   // no WiFi in Zigbee mode
        };
        if (display_init() == ESP_OK) {
            display_show_telemetry(&dt);
            display_deinit();
        }
    }
}
#endif /* USE_ZIGBEE */

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
        case ESP_SLEEP_WAKEUP_GPIO:
            ESP_LOGI(TAG, "Wake cause: GPIO button");
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

#ifndef USE_ZIGBEE
    // Portal mode triggers: GPIO wake (button press) or never-provisioned device.
    // WiFi-build only: the Zigbee build has no WiFi-provisioning concept (commissioning
    // happens over Zigbee), so it must NOT gate on wifi_credentials_is_provisioned()
    // or it would enter the SoftAP portal after a flash erase. Button UX is SP3.
    if (wake_cause == ESP_SLEEP_WAKEUP_GPIO || !wifi_credentials_is_provisioned()) {
        run_portal_then_sleep();
        return;
    }
#endif

#ifdef USE_ZIGBEE
    // Config-portal mode takes priority over the brownout gate: a deliberate
    // button press should always reach config (radio is off, so power is modest).
    // Clear the flag first so a crash mid-portal returns to normal operation.
    if (s_config_request_magic == CONFIG_REQUEST_MAGIC) {
        s_config_request_magic = 0;   // clear first so a crash mid-portal returns to normal
        ESP_LOGI(TAG, "Entering config portal (button-triggered)");
        if (display_init() == ESP_OK) {
            display_show_portal();
            display_deinit();
        }
        config_portal_run();   // blocks until a save handler restarts, or idle timeout
        esp_restart();         // idle-timeout path: reboot back into Zigbee
    }
#endif

    // ------------------------------------------------------------------
    // Zero-load battery sample: must happen before WiFi/MQTT energize.
    // init_system() only touches NVS, event loop, and ADC — no radio yet.
    // ------------------------------------------------------------------
    float ocv = battery_monitor_read_voltage();
    ESP_LOGI(TAG, "OCV: %.3fV (%.0f%% SoC)", ocv, battery_monitor_v_to_pct(ocv));

    if (!battery_monitor_is_safe(ocv)) {
        ESP_LOGE(TAG, "*** LOW BATTERY %.2fV < %.2fV - skipping WiFi/MQTT ***",
                 ocv, BATTERY_LOW_CUTOFF_V);
        if (!s_low_battery_shown) {
            s_low_battery_shown = true;     // latch first, refresh second
            if (display_init() == ESP_OK) {
                display_show_low_battery(ocv);
                display_deinit();
            }
        }
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;
    }
    s_low_battery_shown = false;            // healthy reading clears the latch
    g_cached_battery_v = ocv;               // reused by publish_telemetry_once()

#ifdef USE_ZIGBEE
    // --- Zigbee transport path: managed light-sleep model ---
    // device_id_buffer is normally filled by setup_mqtt() (WiFi path), which the
    // Zigbee path never calls — load it here so the e-paper shows the right ID.
    if (!wifi_credentials_load_device_id(device_id_buffer, sizeof(device_id_buffer))) {
        strncpy(device_id_buffer, DEFAULT_DEVICE_ID, sizeof(device_id_buffer) - 1);
        device_id_buffer[sizeof(device_id_buffer) - 1] = '\0';
    }
    setup_config_button();
    zigbee_reporter_set_location(device_id_buffer);

    // Off-loop report task: the reporter fires a cheap tick on the schedule; the
    // task samples the sensors, pushes the values over Zigbee, and refreshes the
    // e-paper — none of which may run in the stack main loop.
    s_report_sem = xSemaphoreCreateBinary();
    if (s_report_sem != NULL) {
        xTaskCreate(zb_report_task, "zb_report", 4096, NULL, 4, NULL);
        zigbee_reporter_set_report_tick_cb(zb_report_tick);
    } else {
        ESP_LOGE(TAG, "Report semaphore alloc failed — periodic reports disabled");
    }

    zigbee_reporter_set_interval_ms((uint32_t)ZIGBEE_REPORT_INTERVAL_SEC * 1000U);

    if (zigbee_reporter_init() != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee init failed, sleeping");
        enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);
        return;
    }

    if (zigbee_reporter_wait_ready(30000)) {
        ESP_LOGI(TAG, "Zigbee ready (joined)");
    } else {
        ESP_LOGW(TAG, "Zigbee not ready (not joined yet) — stack keeps trying");
    }

    if (ota_client_image_pending_verify()) {
        ESP_LOGW(TAG, "Running a PENDING-VERIFY image — must report once to confirm");
    }

    /* Managed light-sleep: the Zigbee stack task stays alive and pushes periodic
     * reports via the scheduler (periodic_report_cb). app_main returns here;
     * the stack continues running in its own FreeRTOS task.
     * DISABLE_DEEP_SLEEP now only controls whether esp_zb_sleep_enable() is
     * called inside zigbee_reporter.c — no loop or sleep needed here. */
    return;
#else
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

#ifdef DISABLE_DEEP_SLEEP
    ESP_LOGW(TAG, "DISABLE_DEEP_SLEEP set - looping publish every %d ms", TEST_PUBLISH_INTERVAL_MS);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TEST_PUBLISH_INTERVAL_MS));
        publish_telemetry_once();
    }
#else
    // Step 5: Enter deep sleep
    enter_deep_sleep(DEEP_SLEEP_INTERVAL_SEC);

    // This line is never reached - device enters deep sleep
#endif
#endif /* USE_ZIGBEE */
}
