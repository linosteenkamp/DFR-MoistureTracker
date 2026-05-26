#ifndef BATTERY_SOC_H
#define BATTERY_SOC_H

#include <stdbool.h>

/* Cell voltage at or above which the device is allowed to power up
 * WiFi / MQTT for a normal publish cycle. Below this, the firmware
 * skips the cycle and goes back to deep sleep. Chosen to protect the
 * LiPo cell from over-discharge and to match the 20% SoC knee. */
#define BATTERY_LOW_CUTOFF_V  3.70f

/* Convert cell voltage to state-of-charge percentage (0.0–100.0)
 * via piecewise-linear interpolation over an 11-point LiPo curve.
 * Clamps at and below 3.20V to 0.0, at and above 4.20V to 100.0.
 * Defensive: NaN or negative input returns 0.0.
 * Pure function — host-testable, no ESP-IDF dependencies. */
float battery_monitor_v_to_pct(float volts);

/* Returns true iff volts >= BATTERY_LOW_CUTOFF_V.
 * Boundary is inclusive: exactly 3.70V is considered safe. */
bool battery_monitor_is_safe(float volts);

#endif // BATTERY_SOC_H
