// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The state "NVS cannot be used" (see nvs_state.h)

#include "sys/nvs_state.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace
{
    const char *const TAG = "nvs_state";

    // Written once by nvs_state_init() in the start task, before the other tasks that read it are created
    esp_err_t s_error = ESP_ERR_NVS_NOT_INITIALIZED;
} // namespace

esp_err_t nvs_state_init()
{
    // No nvs_flash_erase() on any error, also not on ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND
    s_error = nvs_flash_init();
    if (s_error == ESP_OK)
        return ESP_OK;
    const nvs_state_cause_t cause = nvs_state_classify(s_error);
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NVS_DEFAULT_PART_NAME);
    if (cause != nvs_state_cause_t::no_partition && partition != nullptr)
        ESP_LOGE(TAG, "NVS cannot be used (%s: %s). Nothing is erased; every login is refused and nothing is stored. Recovery: %s: "
                      "esptool --chip %s -p <port> erase-region 0x%lx 0x%lx",
                 esp_err_to_name(s_error), nvs_state_cause_text(cause), nvs_state_recovery_text(cause), CONFIG_IDF_TARGET,
                 static_cast<unsigned long>(partition->address), static_cast<unsigned long>(partition->size));
    else
        ESP_LOGE(TAG, "NVS cannot be used (%s: %s). Nothing is erased; every login is refused and nothing is stored. Recovery: %s",
                 esp_err_to_name(s_error), nvs_state_cause_text(cause), nvs_state_recovery_text(cause));
    return s_error;
}

bool nvs_state_usable()
{
    return s_error == ESP_OK;
}

esp_err_t nvs_state_error()
{
    return s_error;
}
