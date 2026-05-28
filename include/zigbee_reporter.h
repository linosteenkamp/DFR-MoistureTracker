#ifndef ZIGBEE_REPORTER_H
#define ZIGBEE_REPORTER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Start the Zigbee stack as an end-device.
 * Restores persisted network state if already joined, otherwise begins BDB
 * steering (auto-join into any network with permit-join open).
 * Returns ESP_OK once the stack task is running (join completes async). */
esp_err_t zigbee_reporter_init(void);

/* Block until the device is on the network or the timeout elapses.
 * Returns true if joined/ready. */
bool zigbee_reporter_wait_ready(uint32_t timeout_ms);

/* Set + report the sensor attributes. Implemented in Task 4 (stub for now). */
esp_err_t zigbee_reporter_report(float soil_pct, float battery_v, float battery_pct);

#endif // ZIGBEE_REPORTER_H
