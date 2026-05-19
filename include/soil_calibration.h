#ifndef SOIL_CALIBRATION_H
#define SOIL_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Runtime soil-moisture calibration values.
 *
 * Owns the per-device dry/wet mV thresholds and the last-cal timestamp.
 * Backed by NVS namespace "soil_cal" through nvs_shim.
 *
 * Defaults if NVS has no values:
 *   dry_mv = 2800, wet_mv = 0, cal_ts = 0
 */

/** Load from NVS into RAM. Falls back to defaults on missing keys. */
void soil_calibration_init(void);

uint32_t soil_calibration_get_dry_mv(void);
uint32_t soil_calibration_get_wet_mv(void);
uint32_t soil_calibration_get_cal_ts(void);

/** Persist values to NVS and update in-RAM cache. */
bool soil_calibration_save(uint32_t dry_mv, uint32_t wet_mv, uint32_t cal_ts);

/** Erase the whole "soil_cal" namespace. RAM cache reverts on next init. */
bool soil_calibration_clear(void);

#endif
