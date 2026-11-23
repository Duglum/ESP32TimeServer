// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// See nvs_purge.h (replaced and erased NVS data is purged from the flash)

#include "sys/nvs_purge.h"
#include "sys/secret.h"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sys/nvs_state.h"

#if CONFIG_NVS_ENCRYPTION
#error "nvs_purge_start() reads and writes the NVS pages in clear text: not for an encrypted NVS partition"
#endif

namespace
{
    const char *const TAG = "nvs_purge";

    size_t entries_of(const nvs_purge_run_t runs[], size_t count)
    {
        size_t bytes = 0;
        for (size_t i = 0; i < count; ++i)
            bytes += runs[i].size;
        return bytes / 32;
    }
} // namespace

void nvs_purge_start()
{
    if (!nvs_state_usable())
        return;
    const int64_t start_us = esp_timer_get_time();
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NVS_DEFAULT_PART_NAME);
    uint8_t *page = static_cast<uint8_t *>(malloc(NVS_PURGE_PAGE_SIZE));
    uint8_t *zeros = static_cast<uint8_t *>(calloc(1, NVS_PURGE_PAGE_SIZE));
    nvs_purge_run_t *runs = static_cast<nvs_purge_run_t *>(malloc(NVS_PURGE_MAX_RUNS * sizeof(nvs_purge_run_t)));
    if (partition == nullptr || page == nullptr || zeros == nullptr || runs == nullptr)
    {
        ESP_LOGW(TAG, "Erased NVS data could not be purged (%s)", partition == nullptr ? "no NVS partition" : "no memory");
        free(page);
        free(zeros);
        free(runs);
        return;
    }
    size_t zeroed = 0;
    size_t left = 0;
    unsigned pages = 0;
    unsigned failed = 0;
    for (size_t offset = 0; offset + NVS_PURGE_PAGE_SIZE <= partition->size; offset += NVS_PURGE_PAGE_SIZE)
    {
        if (esp_partition_read(partition, offset, page, NVS_PURGE_PAGE_SIZE) != ESP_OK)
        {
            ++failed;
            continue;
        }
        const size_t count = nvs_purge_page_runs(page, runs);
        if (count == 0)
            continue;
        bool written = true;
        for (size_t i = 0; i < count; ++i)
            written = esp_partition_write(partition, offset + runs[i].offset, zeros, runs[i].size) == ESP_OK && written;
        // Read again: what is left (a write that failed)
        if (esp_partition_read(partition, offset, page, NVS_PURGE_PAGE_SIZE) != ESP_OK)
        {
            ++failed;
            continue;
        }
        const size_t entries = entries_of(runs, count);
        const size_t still = entries_of(runs, nvs_purge_page_runs(page, runs));
        zeroed += entries - still;
        left += still;
        ++pages;
        if (!written)
            ++failed;
    }
    mbedtls_platform_zeroize(page, NVS_PURGE_PAGE_SIZE); // it held stored data, secrets among them
    free(page);
    free(zeros);
    free(runs);
    ESP_LOGI(TAG, "Erased NVS data: %u entries zeroed on %u pages, %lld ms", static_cast<unsigned>(zeroed), pages,
             static_cast<long long>((esp_timer_get_time() - start_us) / 1000));
    if (left > 0 || failed > 0)
        ESP_LOGW(TAG, "Erased NVS data: %u entries still hold data, %u pages could not be read or written", static_cast<unsigned>(left), failed);
}
