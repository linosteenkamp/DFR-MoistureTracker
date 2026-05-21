#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifndef TEST_HOST
#include "esp_err.h"
#endif

/**
 * @brief Telemetry values to render on the dashboard view.
 *
 * All fields are owned by the caller; the renderer only reads them.
 * Strings must remain valid for the duration of the display_show_telemetry call.
 */
typedef struct {
    const char *device_id;
    float       moisture_pct;
    int         raw_mv;
    float       battery_v;
    int         battery_pct;
    int         wifi_rssi_dbm;   // 0 = unknown / not connected
} display_telemetry_t;

#ifndef TEST_HOST
/** Initialise SPI bus, GPIOs, wake panel from deep sleep. */
esp_err_t display_init(void);

/** Render the dashboard view in a fresh full refresh. */
void display_show_telemetry(const display_telemetry_t *t);

/** Render the portal view (SSID + URL + QR) in a fresh full refresh. */
void display_show_portal(void);

/** Put the panel back into deep sleep and release the SPI bus. */
void display_deinit(void);
#endif

/** Pure: 3.3 V = 0 %, 4.2 V = 100 %, clamped. No hardware access. */
int display_battery_v_to_pct(float v);

#endif // DISPLAY_H
