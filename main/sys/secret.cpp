// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// See secret.h (memory that held secrets is zeroed before it is freed)

#include "sys/secret.h"

#include <cstdlib>

#include "esp_heap_caps.h"

void secret_free(void *block)
{
    if (block == nullptr)
        return;
    mbedtls_platform_zeroize(block, heap_caps_get_allocated_size(block));
    free(block);
}
