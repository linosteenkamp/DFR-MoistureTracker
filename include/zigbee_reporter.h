#ifndef ZIGBEE_REPORTER_H
#define ZIGBEE_REPORTER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Fills the three values with a fresh sensor sample. Called from the Zigbee
 * stack context on the report schedule. */
typedef void (*zigbee_sample_cb_t)(float *soil_pct, float *battery_v, float *battery_pct);

/* Register the sampling callback and the report interval (milliseconds).
 * Must be called before zigbee_reporter_init(). */
void zigbee_reporter_set_sample_cb(zigbee_sample_cb_t cb);
void zigbee_reporter_set_interval_ms(uint32_t interval_ms);

/* Start the Zigbee stack as an end-device.
 * Restores persisted network state if already joined, otherwise begins BDB
 * steering (auto-join into any network with permit-join open).
 * Returns ESP_OK once the stack task is running (join completes async). */
esp_err_t zigbee_reporter_init(void);

/* Block until the device is on the network or the timeout elapses.
 * Returns true if joined/ready. */
bool zigbee_reporter_wait_ready(uint32_t timeout_ms);

/* Set + report the sensor attributes from outside the stack task (takes the
 * Zigbee lock). For use from other FreeRTOS tasks only — NOT from scheduler
 * callbacks (use the no-lock path inside zigbee_reporter.c instead). */
esp_err_t zigbee_reporter_report(float soil_pct, float battery_v, float battery_pct);

#endif // ZIGBEE_REPORTER_H
