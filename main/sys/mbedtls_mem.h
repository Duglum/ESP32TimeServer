// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The memory functions of mbedTLS. With CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC ESP-IDF 6.1.1 compiles neither its own
// esp_mbedtls_mem_calloc() / esp_mbedtls_mem_free() (port/esp_mem.c) nor points mbedTLS at them (esp_config.h): until
// mbedtls_platform_set_calloc_free() is called, mbedTLS takes calloc() and free() of the C library
// (tf-psa-crypto platform.h, platform.c). So the firmware calls mbedtls_mem_install() first in app_main(), before
// anything uses mbedTLS. A block taken before it (with calloc()) is freed through esp_mbedtls_mem_free() as well:
// secret_free() handles every block of the heap.
#pragma once

// mbedTLS gets esp_mbedtls_mem_calloc() and esp_mbedtls_mem_free() of mbedtls_mem.cpp
void mbedtls_mem_install();
