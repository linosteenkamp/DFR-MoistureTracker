#include "soil_calibration.h"
#include "nvs_shim.h"

#define NS          "soil_cal"
#define KEY_DRY     "dry_mv"
#define KEY_WET     "wet_mv"
#define KEY_TS      "cal_ts"

#define DEFAULT_DRY 2800
#define DEFAULT_WET 0
#define DEFAULT_TS  0

static uint32_t s_dry = DEFAULT_DRY;
static uint32_t s_wet = DEFAULT_WET;
static uint32_t s_ts  = DEFAULT_TS;

void soil_calibration_init(void) {
    uint32_t v;
    s_dry = nvs_shim_get_u32(NS, KEY_DRY, &v) ? v : DEFAULT_DRY;
    s_wet = nvs_shim_get_u32(NS, KEY_WET, &v) ? v : DEFAULT_WET;
    s_ts  = nvs_shim_get_u32(NS, KEY_TS,  &v) ? v : DEFAULT_TS;
}

uint32_t soil_calibration_get_dry_mv(void) { return s_dry; }
uint32_t soil_calibration_get_wet_mv(void) { return s_wet; }
uint32_t soil_calibration_get_cal_ts(void) { return s_ts;  }

// Not atomic across the three keys — a mid-save NVS failure leaves the
// namespace partially updated, and the in-RAM cache stays stale until the
// next init re-reads NVS and defaults whichever keys are missing.
bool soil_calibration_save(uint32_t dry_mv, uint32_t wet_mv, uint32_t cal_ts) {
    if (!nvs_shim_set_u32(NS, KEY_DRY, dry_mv)) return false;
    if (!nvs_shim_set_u32(NS, KEY_WET, wet_mv)) return false;
    if (!nvs_shim_set_u32(NS, KEY_TS,  cal_ts)) return false;
    s_dry = dry_mv; s_wet = wet_mv; s_ts = cal_ts;
    return true;
}

bool soil_calibration_clear(void) {
    return nvs_shim_erase_namespace(NS);
}
