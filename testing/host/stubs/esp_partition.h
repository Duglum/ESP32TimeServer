// ESP32-WebTherm host tests: what main/sys/nvs_purge.cpp uses of ESP-IDF's esp_partition.h (the test defines the calls:
// a fake NVS partition)
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

typedef enum
{
    ESP_PARTITION_TYPE_DATA = 0x01,
} esp_partition_type_t;

typedef enum
{
    ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02,
} esp_partition_subtype_t;

typedef struct
{
    uint32_t address;
    uint32_t size;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst, size_t size);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size);
