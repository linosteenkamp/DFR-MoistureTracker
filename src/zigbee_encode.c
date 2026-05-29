#include "zigbee_encode.h"
#include <math.h>

uint16_t zigbee_encode_soil_pct(float pct) {
    if (isnan(pct) || pct <= 0.0f) return 0;
    if (pct >= 100.0f) return 10000;
    return (uint16_t)lroundf(pct * 100.0f);
}

uint8_t zigbee_encode_batt_voltage(float volts) {
    if (isnan(volts) || volts <= 0.0f) return 0;
    long units = lroundf(volts * 10.0f);
    if (units > 255) return 255;
    return (uint8_t)units;
}

uint8_t zigbee_encode_batt_pct(float pct) {
    if (isnan(pct) || pct <= 0.0f) return 0;
    if (pct >= 100.0f) return 200;
    return (uint8_t)lroundf(pct * 2.0f);
}
