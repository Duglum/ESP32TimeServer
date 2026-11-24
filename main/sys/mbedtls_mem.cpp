// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Memory of mbedTLS (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC, see mbedtls_mem.h): internal RAM as ESP-IDF's "Internal memory"
// mode gives it (esp_mem.c of ESP-IDF 6.1.1), and every block zeroed before it is freed (sys/secret.h).

#include "sys/mbedtls_mem.h"

#include <cstddef>

#include "esp_heap_caps.h"
#include "sys/secret.h"

extern "C"
{
#include "mbedtls/platform.h"
}

extern "C" void *esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// With ESP-IDF's dynamic TLS buffers (CONFIG_MBEDTLS_DYNAMIC_BUFFER) a record buffer is freed through here right
// after its record was read (esp_mbedtls_free_buf() of ESP-IDF 6.1.1 calls mbedtls_free() without zeroing it): it
// holds the decrypted request, with the passwords, TOTP codes and private keys of the page among them.
extern "C" void esp_mbedtls_mem_free(void *ptr)
{
    secret_free(ptr);
}

void mbedtls_mem_install()
{
    mbedtls_platform_set_calloc_free(esp_mbedtls_mem_calloc, esp_mbedtls_mem_free);
}
