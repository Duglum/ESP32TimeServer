// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The presence marks in NVS (see record_mark.h)

#include "sys/record_mark.h"

#include "esp_log.h"
#include "nvs.h"

[[noreturn]] void app_restart_not_kept(const char *reason); // main.cpp (declared in web/web_settings.h)

namespace
{
    const char *const TAG = "record_mark";

    // The stored mark: ESP_OK (*value set), ESP_ERR_NVS_NOT_FOUND (none), or another error of the read
    esp_err_t read_mark(const char *mark, uint64_t *value)
    {
        *value = 0;
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(RECORD_MARK_NVS_NAMESPACE, NVS_READONLY, &handle);
        if (err != ESP_OK)
            return err;
        err = nvs_get_u64(handle, mark, value);
        nvs_close(handle);
        return err;
    }
} // namespace

record_mark_state_t record_mark_check(const char *ns, const char *key, bool record_valid, uint64_t value, uint64_t *stored)
{
    if (stored != nullptr)
        *stored = 0;
    char mark[RECORD_MARK_KEY_SIZE];
    if (!record_mark_key(ns, key, mark))
    {
        ESP_LOGE(TAG, "No mark key for %s/%s (too long)", ns, key);
        return record_valid ? record_mark_state_t::present : record_mark_state_t::none;
    }
    uint64_t found = 0;
    const esp_err_t err = read_mark(mark, &found);
    // Without memory for the handle (nvs_open() allocates it) the mark is not known: counted as a mark, a record that
    // was never stored would read as lost for the whole run; counted as none, a lost one would read as never stored
    // (a default in its place, e.g. the default password). Neither: a restart (the callers run at the start)
    if (err == ESP_ERR_NO_MEM)
    {
        ESP_LOGE(TAG, "The mark %s could not be read (out of memory): restart", mark);
        app_restart_not_kept("nvs_no_memory");
    }
    bool marked = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        // The namespace opened but the mark cannot be read: counted as a mark (a record that is not valid then reads
        // as lost, not as never stored). When NVS itself is not usable, nothing can be read: no mark, and no line for
        // each record (the firmware is in the state of nvs_state.h, which refuses every login by itself and logs it).
        marked = err != ESP_ERR_NVS_NOT_INITIALIZED && err != ESP_ERR_NVS_PART_NOT_FOUND;
        if (marked)
            ESP_LOGE(TAG, "The mark %s could not be read (%s)", mark, esp_err_to_name(err));
    }
    if (stored != nullptr)
        *stored = found;
    const record_mark_state_t state = record_mark_decide(marked, record_valid);
    if (state == record_mark_state_t::adopted || (state == record_mark_state_t::present && found != value))
    {
        const esp_err_t set_err = record_mark_set(ns, key, value);
        if (set_err != ESP_OK)
            ESP_LOGW(TAG, "The mark %s could not be stored (%s)", mark, esp_err_to_name(set_err));
        else if (state == record_mark_state_t::adopted)
            ESP_LOGI(TAG, "Mark %s set for the stored record", mark);
    }
    return state;
}

esp_err_t record_mark_set(const char *ns, const char *key, uint64_t value)
{
    char mark[RECORD_MARK_KEY_SIZE];
    if (!record_mark_key(ns, key, mark))
        return ESP_ERR_INVALID_ARG;
    uint64_t found = 0;
    if (read_mark(mark, &found) == ESP_OK && found == value)
        return ESP_OK;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(RECORD_MARK_NVS_NAMESPACE, NVS_READWRITE_PURGE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u64(handle, mark, value);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t record_mark_clear(const char *ns, const char *key)
{
    char mark[RECORD_MARK_KEY_SIZE];
    if (!record_mark_key(ns, key, mark))
        return ESP_ERR_INVALID_ARG;
    uint64_t found = 0;
    esp_err_t err = read_mark(mark, &found);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK; // no mark (and no namespace is made for nothing)
    nvs_handle_t handle = 0;
    err = nvs_open(RECORD_MARK_NVS_NAMESPACE, NVS_READWRITE_PURGE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_key(handle, mark);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t record_mark_clear_all()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(RECORD_MARK_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (err != ESP_OK)
        return err;
    nvs_close(handle);
    err = nvs_open(RECORD_MARK_NVS_NAMESPACE, NVS_READWRITE_PURGE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
