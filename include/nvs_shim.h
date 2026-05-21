#ifndef NVS_SHIM_H
#define NVS_SHIM_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Thin wrapper around ESP NVS u32 get/set + namespace erase.
 *
 * Exists so modules that only need u32 storage (e.g. soil_calibration)
 * can be unit-tested on the host by linking against an in-memory stub
 * implementation. The ESP build links nvs_shim_esp.c; native tests
 * link an in-test stub defined alongside the test file.
 */

/** Get a u32 value. Returns false if namespace/key absent. */
bool nvs_shim_get_u32(const char *ns, const char *key, uint32_t *out);

/** Set a u32 value, commit immediately. Returns false on failure. */
bool nvs_shim_set_u32(const char *ns, const char *key, uint32_t value);

/** Erase all keys in a namespace. Returns false on failure. */
bool nvs_shim_erase_namespace(const char *ns);

#endif
