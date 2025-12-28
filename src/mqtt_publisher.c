#include "mqtt_publisher.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "MQTT_PUB";
static esp_mqtt_client_handle_t client = NULL;
static bool mqtt_connected = false;
static const char *base_topic = NULL;

// MQTT event handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            mqtt_connected = true;
            // Publish online status
            // if (client && base_topic) {
            //     esp_mqtt_client_publish(client, base_topic, "online", 0, 1, 0);
            // }
            // break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected");
            mqtt_connected = false;
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            break;
            
        default:
            break;
    }
}

esp_err_t mqtt_publisher_init(const mqtt_config_t *config) {
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing MQTT client");
    
    base_topic = config->base_topic;
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->broker_uri,
        .credentials.username = config->username,
        .credentials.authentication.password = config->password,
        .session.keepalive = config->keepalive_sec,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, 
                                                     mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler");
        return err;
    }
    
    err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        return err;
    }
    
    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

bool mqtt_publisher_is_connected(void) {
    return mqtt_connected;
}

esp_err_t mqtt_publisher_publish_telemetry(float battery_voltage, float soil_moisture, const char *device_name) {
    if (!client || !mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, skipping publish");
        return ESP_FAIL;
    }
    
    if (!base_topic) {
        ESP_LOGE(TAG, "Base topic not configured");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Format JSON payload
    char payload[192];
    int len = snprintf(payload, sizeof(payload), 
                      "{\"battery\":%.2f,\"soil_moisture\":%.1f,\"device\":\"%s\"}", 
                      battery_voltage, soil_moisture, device_name);
    
    if (len < 0 || len >= sizeof(payload)) {
        ESP_LOGE(TAG, "Failed to format payload");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Publishing: %s", payload);
    
    int msg_id = esp_mqtt_client_publish(client, base_topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish message");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}
