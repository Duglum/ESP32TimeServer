// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Core dump of the last crash, see crash_dump.h

#include "custom/crash_dump.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"

namespace
{
    const char *TAG = "crash_dump";

    // Written at the start and by an erase (both before or in the web server task), read by the web server task
    crash_dump_info_t s_info{};
    size_t s_flash_address = 0;
}

void crash_dump_init()
{
    s_info = crash_dump_info_t{};
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr) == nullptr)
    {
        ESP_LOGW(TAG, "No core dump partition: crashes are not recorded (flash the partition table once over USB)");
        return;
    }
    size_t address = 0;
    size_t size = 0;
    const esp_err_t found = esp_core_dump_image_get(&address, &size);
    if (found != ESP_OK)
    {
        if (found != ESP_ERR_NOT_FOUND) // ESP_ERR_NOT_FOUND: the partition is blank, no crash so far
            ESP_LOGW(TAG, "The core dump partition cannot be read (%s)", esp_err_to_name(found));
        return;
    }
    const esp_err_t valid = esp_core_dump_image_check();
    if (valid != ESP_OK)
    {
        if (valid != ESP_ERR_NOT_FOUND)
            ESP_LOGW(TAG, "The stored core dump is damaged (%s)", esp_err_to_name(valid));
        return;
    }

    s_info.present = true;
    s_info.size = static_cast<uint32_t>(size);
    s_flash_address = address;
    // The summary holds a copy of the stack of the crashed task (about 1 kB): on the heap
    esp_core_dump_summary_t *summary = new (std::nothrow) esp_core_dump_summary_t{};
    if (summary != nullptr && esp_core_dump_get_summary(summary) == ESP_OK)
    {
        snprintf(s_info.task, sizeof(s_info.task), "%.*s", static_cast<int>(sizeof(summary->exc_task)), summary->exc_task);
        s_info.pc = summary->exc_pc;
    }
    delete summary;
    if (esp_core_dump_get_panic_reason(s_info.reason, sizeof(s_info.reason)) != ESP_OK)
        s_info.reason[0] = '\0';
    ESP_LOGW(TAG, "A core dump of a crash is stored: task %s, PC 0x%08lx%s%s (%lu bytes; download it on the web admin page or with idf.py coredump-info)",
             s_info.task[0] != '\0' ? s_info.task : "?", static_cast<unsigned long>(s_info.pc), s_info.reason[0] != '\0' ? ", " : "",
             s_info.reason, static_cast<unsigned long>(s_info.size));
#endif
}

void crash_dump_get(crash_dump_info_t *info)
{
    if (info != nullptr)
        *info = s_info;
}

bool crash_dump_read(size_t offset, void *buffer, size_t length)
{
    if (!s_info.present || buffer == nullptr || offset > s_info.size || length > s_info.size - offset)
        return false;
    return esp_flash_read(nullptr, buffer, static_cast<uint32_t>(s_flash_address + offset), static_cast<uint32_t>(length)) == ESP_OK;
}

bool crash_dump_erase()
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (!s_info.present)
        return true;
    if (esp_core_dump_image_erase() != ESP_OK)
        return false;
    s_info = crash_dump_info_t{};
    ESP_LOGI(TAG, "Core dump erased");
    return true;
#else
    return false;
#endif
}
