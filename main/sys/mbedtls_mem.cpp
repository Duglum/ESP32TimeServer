// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Memory of mbedTLS (CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC, see mbedtls_mem.h): internal RAM as ESP-IDF's "Internal memory"
// mode gives it (esp_mem.c of ESP-IDF 6.1.1), and every block zeroed before it is freed (sys/secret.h).

#include "sys/mbedtls_mem.h"

#include <atomic>
#include <cstddef>

#include "esp_heap_caps.h"
#include "sys/secret.h"

extern "C"
{
#include "mbedtls/platform.h"
}

namespace
{
    std::atomic<unsigned> s_failures{0};
} // namespace

extern "C" void *esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    void *block = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (block == nullptr && n != 0 && size != 0)
        s_failures.fetch_add(1, std::memory_order_relaxed);
    return block;
}

unsigned mbedtls_mem_failures()
{
    return s_failures.load(std::memory_order_relaxed);
}

// With ESP-IDF's dynamic TLS buffers (CONFIG_MBEDTLS_DYNAMIC_BUFFER) a record buffer is freed through here
// (esp_mbedtls_free_buf() of ESP-IDF 6.1.1 calls mbedtls_free() and does not zero it). mbedTLS zeroes the application
// data it hands to the web server (mbedtls_ssl_read()), so such a buffer still holds decrypted data only when the
// connection ends before it was read: a request header above the limit of the web server (answered 431), the rest of
// a request the web server cuts off.
extern "C" void esp_mbedtls_mem_free(void *ptr)
{
    secret_free(ptr);
}

void mbedtls_mem_install()
{
    mbedtls_platform_set_calloc_free(esp_mbedtls_mem_calloc, esp_mbedtls_mem_free);
}
