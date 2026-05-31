set(SRCS
    "adc_manager.c"
    "battery_monitor.c"
    "config_portal.c"
    "display.c"
    "form_parser.c"
    "main.c"
    "mqtt_publisher.c"
    "nvs_shim_esp.c"
    "ota_client.c"
    "soil_calibration.c"
    "soil_moisture.c"
    "wifi_credentials.c"
    "wifi_manager.c"
    "zigbee_encode.c"
    "zigbee_reporter.c"
)

# The managed component include path is added unconditionally so it resolves
# in the zigbee env (where USE_ZIGBEE is defined and the headers are needed).
# In the WiFi env the entire zigbee_reporter.c body is gated by #ifdef USE_ZIGBEE,
# so no actual Zigbee symbols are referenced and no linker dependency is added.
set(ZB_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp-zigbee-lib/include"
)

# Unconditionally declare the managed-component dependencies.
# idf_component_register REQUIRES is the correct mechanism — the components
# exist in managed_components/ whether or not CONFIG_ZB_ENABLED is set.
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "."
        "../include"
        ${ZB_INCLUDE_DIR}
    REQUIRES
        espressif__esp-zigbee-lib
        espressif__esp-zboss-lib
)
