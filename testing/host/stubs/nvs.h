// ESP32 Time Server - host tests: the NVS calls of main/custom/nvs_blob.cpp (declarations and error codes as in
// ESP-IDF 6.1.1 components/nvs_flash/include/nvs.h). The test that links nvs_blob.cpp defines them (a fake NVS).
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef uint32_t nvs_handle_t;
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_HANDLE (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_INVALID_LENGTH (ESP_ERR_NVS_BASE + 0x0c)

    esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
    esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);

#ifdef __cplusplus
}
#endif
