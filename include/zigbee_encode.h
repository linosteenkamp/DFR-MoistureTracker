#ifndef ZIGBEE_ENCODE_H
#define ZIGBEE_ENCODE_H

#include <stdint.h>

/* Soil moisture percent (0-100) -> ZCL Soil Moisture MeasuredValue.
 * uint16 in 0.01% units, range 0..10000. Clamps out-of-range; NaN/neg -> 0. */
uint16_t zigbee_encode_soil_pct(float pct);

/* Battery volts -> ZCL Power Config BatteryVoltage.
 * uint8 in 100mV units. Caps at 255; NaN/neg -> 0. */
uint8_t zigbee_encode_batt_voltage(float volts);

/* Battery percent (0-100) -> ZCL Power Config BatteryPercentageRemaining.
 * uint8 in 0.5% units, range 0..200. Clamps out-of-range; NaN/neg -> 0. */
uint8_t zigbee_encode_batt_pct(float pct);

#endif // ZIGBEE_ENCODE_H
