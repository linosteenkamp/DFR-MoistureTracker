#include "nvs_shim.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "NVS_SHIM";

bool nvs_shim_get_u32(const char *ns, const char *key, uint32_t *out) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_u32(h, key, out);
    nvs_close(h);
    return err == ESP_OK;
}

bool nvs_shim_set_u32(const char *ns, const char *key, uint32_t value) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "open %s/%s failed", ns, key);
        return false;
    }
    bool ok = (nvs_set_u32(h, key, value) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool nvs_shim_erase_namespace(const char *ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = (nvs_erase_all(h) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}
