#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifndef TEST_HOST
#include "esp_err.h"
#endif

/**
 * @brief Pure: linear voltage -> battery percentage. 3.3 V = 0, 4.2 V = 100.
 *        Clamped to [0, 100]. No hardware access.
 */
int display_battery_v_to_pct(float v);

#endif // DISPLAY_H
