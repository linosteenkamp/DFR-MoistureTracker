#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include "esp_err.h"

/**
 * @brief Run the SoftAP configuration portal, blocking.
 *
 * Brings up the FireBeetle_C6_Prov AP and an HTTP server. Serves the
 * config menu (WiFi, calibration, status, factory reset). Returns when:
 *  - A handler calls esp_restart() (it never returns), or
 *  - The 10-minute idle timeout fires (returns ESP_OK).
 *
 * Caller is expected to deep-sleep afterwards.
 */
esp_err_t config_portal_run(void);

#endif
