// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// See nvs_blob.h (workaround for the multi-chunk compare of ESP-IDF 6.1.1 nvs_set_blob())

#include "custom/nvs_blob.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    // The copy may hold secrets: a volatile store loop, which the compiler cannot drop before free()
    void wipe(void *data, size_t size)
    {
        volatile uint8_t *p = static_cast<volatile uint8_t *>(data);
        while (size-- > 0)
            *p++ = 0;
    }

    // True if NVS holds exactly size bytes equal to data under key
    bool stored_equal(nvs_handle_t handle, const char *key, const void *data, size_t size)
    {
        size_t length = 0;
        if (nvs_get_blob(handle, key, nullptr, &length) != ESP_OK || length != size || size == 0)
            return false;
        uint8_t *stored = static_cast<uint8_t *>(malloc(size));
        if (stored == nullptr)
            return false;
        bool equal = nvs_get_blob(handle, key, stored, &length) == ESP_OK && length == size && memcmp(stored, data, size) == 0;
        wipe(stored, size);
        free(stored);
        return equal;
    }
} // namespace

esp_err_t nvs_blob_set_if_changed(nvs_handle_t handle, const char *key, const void *data, size_t size, bool *written)
{
    if (written != nullptr)
        *written = false;
    if (stored_equal(handle, key, data, size))
        return ESP_OK;
    const esp_err_t err = nvs_set_blob(handle, key, data, size);
    if (written != nullptr)
        *written = err == ESP_OK;
    return err;
}
