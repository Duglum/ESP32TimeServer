// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE). From ESP32-WebTherm (sys/cjson_mem.cpp of a3fb9aa), adapted.

#include "sys/cjson_mem.h"
#include "sys/secret.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>

#include "cJSON.h"

extern "C"
{
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace
{
    std::atomic<TaskHandle_t> s_watched{nullptr};
    std::atomic<bool> s_failed{false};

    void *allocate(size_t size)
    {
        void *block = malloc(size);
        if (block == nullptr && xTaskGetCurrentTaskHandle() == s_watched.load(std::memory_order_relaxed))
            s_failed.store(true, std::memory_order_relaxed);
        return block;
    }
} // namespace

void cjson_mem_init()
{
    // cJSON takes its nodes from malloc(), as it did without a hook (internal RAM on these boards). Every block it
    // frees is zeroed first (sys/secret.h): the trees of requests hold passwords, shared secrets and keys, and so do
    // printed texts (the configuration export) and the buffers of a print (with a free function other than free()
    // cJSON has no realloc and copies a growing print buffer into a new block, freeing the old one through this
    // function: cJSON.c cJSON_InitHooks(), ensure()). cJSON is used by the web page only (and auth/auth_config.cpp for
    // it), never on the NTP or PTP path.
    cJSON_Hooks hooks{};
    hooks.malloc_fn = allocate;
    hooks.free_fn = secret_free;
    cJSON_InitHooks(&hooks);
}

void cjson_mem_watch_begin()
{
    s_watched.store(xTaskGetCurrentTaskHandle(), std::memory_order_relaxed);
    s_failed.store(false, std::memory_order_relaxed);
}

bool cjson_mem_failed()
{
    return s_failed.load(std::memory_order_relaxed) && xTaskGetCurrentTaskHandle() == s_watched.load(std::memory_order_relaxed);
}
