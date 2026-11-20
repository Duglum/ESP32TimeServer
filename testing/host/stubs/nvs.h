// ESP32 Time Server - host tests: the NVS calls of main/custom/nvs_blob.cpp and main/sys/record_mark.cpp (declarations
// and error codes as in ESP-IDF 6.1.1 components/nvs_flash/include/nvs.h). The tests that link them define them (a
// fake NVS).
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
#define ESP_ERR_NVS_NOT_INITIALIZED (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_TYPE_MISMATCH (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_PART_NOT_FOUND (ESP_ERR_NVS_BASE + 0x0f)

    typedef enum
    {
        NVS_READONLY,
        NVS_READWRITE,
        NVS_READWRITE_PURGE
    } nvs_open_mode_t;

    esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
    esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
    // main/sys/record_mark.cpp (record_mark_test defines them)
    esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
    void nvs_close(nvs_handle_t handle);
    esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *out_value);
    esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value);
    esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
    esp_err_t nvs_erase_all(nvs_handle_t handle);
    esp_err_t nvs_commit(nvs_handle_t handle);

#ifdef __cplusplus
}
#endif
