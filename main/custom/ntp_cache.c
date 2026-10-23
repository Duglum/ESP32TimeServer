// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "ntp_cache.h"
#include <stdatomic.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// 0 = distinguish clients by IP address only (interleaved clients such as chrony use a new source port
//     for every request, so a port-keyed cache would never find the previous exchange)
// 1 = distinguish clients by IP address and source port
#define NTP_CACHE_MATCH_PORT 0

// Per-client rate limit: a client may send this far ahead of its sustained rate, i.e. a burst of up to one second
// of requests (the limit per second). It also absorbs processing delays of the NTP tasks (e.g. a flash write), after
// which queued requests of a conforming client are handled in quick succession.
#define RATE_LIMIT_BURST_US 1000000

static ntp_client_record_t s_ipv4_records[MAX_TRACKED_CLIENTS_IPV4];
static ntp_client_record_ipv6_t s_ipv6_records[MAX_TRACKED_CLIENTS_IPV6];
static SemaphoreHandle_t s_mutex;
static _Atomic uint32_t s_rate_interval_us; // 1 s / rate limit, 0 = no rate limit

static int64_t current_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Must be called with s_mutex held.
// Generic cell rate algorithm (virtual scheduling): *tat_us is the theoretical arrival time of the next request.
// Each answered request moves it on by the interval (1 s / limit); a request that arrives more than
// RATE_LIMIT_BURST_US - interval ahead of it is over the limit and leaves it unchanged. The sustained rate is
// exactly the limit, with a burst of up to one second worth of requests, and there are no window boundaries at which
// a conforming client could be counted twice. Costs one clock reading, a compare and an add.
static bool rate_limit_exceeded(int64_t *tat_us)
{
    const uint32_t interval_us = atomic_load_explicit(&s_rate_interval_us, memory_order_relaxed);
    if (interval_us == 0)
    {
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    if (*tat_us - now_us > (int64_t)RATE_LIMIT_BURST_US - (int64_t)interval_us)
    {
        return true;
    }
    *tat_us = (*tat_us > now_us ? *tat_us : now_us) + interval_us;
    return false;
}

static bool port_matches(uint16_t record_port, uint16_t port)
{
#if NTP_CACHE_MATCH_PORT
    return record_port == port;
#else
    (void)record_port;
    (void)port;
    return true;
#endif
}

// Must be called with s_mutex held.
// Returns the index of the matching record (*found = true), or the index of the record to be used for
// a new client (a free slot, otherwise the oldest record).
static size_t select_ipv4_slot(uint32_t ip, uint16_t port, bool *found)
{
    size_t selected = 0;
    int64_t oldest_ms = INT64_MAX;
    *found = false;

    for (size_t i = 0; i < MAX_TRACKED_CLIENTS_IPV4; ++i)
    {
        if (s_ipv4_records[i].is_active && s_ipv4_records[i].client_ip == ip &&
            port_matches(s_ipv4_records[i].client_port, port))
        {
            *found = true;
            return i;
        }
        if (!s_ipv4_records[i].is_active)
        {
            selected = i;
            oldest_ms = INT64_MIN;
        }
        else if (oldest_ms != INT64_MIN && s_ipv4_records[i].last_seen_ms < oldest_ms)
        {
            selected = i;
            oldest_ms = s_ipv4_records[i].last_seen_ms;
        }
    }
    return selected;
}

static size_t select_ipv6_slot(const struct in6_addr *ip, uint16_t port, bool *found)
{
    size_t selected = 0;
    int64_t oldest_ms = INT64_MAX;
    *found = false;

    for (size_t i = 0; i < MAX_TRACKED_CLIENTS_IPV6; ++i)
    {
        if (s_ipv6_records[i].is_active && port_matches(s_ipv6_records[i].client_port, port) &&
            memcmp(&s_ipv6_records[i].client_ip, ip, sizeof(*ip)) == 0)
        {
            *found = true;
            return i;
        }
        if (!s_ipv6_records[i].is_active)
        {
            selected = i;
            oldest_ms = INT64_MIN;
        }
        else if (oldest_ms != INT64_MIN && s_ipv6_records[i].last_seen_ms < oldest_ms)
        {
            selected = i;
            oldest_ms = s_ipv6_records[i].last_seen_ms;
        }
    }
    return selected;
}

bool ntp_cache_init(void)
{
    if (s_mutex != NULL)
    {
        return true;
    }
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

void ntp_cache_deinit(void)
{
    if (s_mutex != NULL)
    {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
}

void ntp_cache_set_rate_limit(uint32_t requests_per_second)
{
    atomic_store_explicit(&s_rate_interval_us, requests_per_second == 0 ? 0u : 1000000u / requests_per_second,
                          memory_order_relaxed);
}

bool ntp_cache_find_or_create(uint32_t ip, uint16_t port, ntp_client_record_t *record, bool *rate_limited)
{
    if (rate_limited != NULL)
    {
        *rate_limited = false;
    }
    if (s_mutex == NULL || record == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    bool found = false;
    const size_t selected = select_ipv4_slot(ip, port, &found);

    if (!found)
    {
        memset(&s_ipv4_records[selected], 0, sizeof(s_ipv4_records[selected]));
        s_ipv4_records[selected].client_ip = ip;
        s_ipv4_records[selected].is_active = true;
        s_ipv4_records[selected].last_seen_ms = current_time_ms();
    }
    s_ipv4_records[selected].client_port = port;
    if (rate_limited != NULL)
    {
        *rate_limited = rate_limit_exceeded(&s_ipv4_records[selected].rate_tat_us);
    }

    *record = s_ipv4_records[selected];
    xSemaphoreGive(s_mutex);
    return true;
}

bool ntp_cache_update(uint32_t ip, uint16_t port, uint64_t t2, uint64_t t3)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    bool found = false;
    const size_t selected = select_ipv4_slot(ip, port, &found);

    if (!found)
    {
        memset(&s_ipv4_records[selected], 0, sizeof(s_ipv4_records[selected]));
        s_ipv4_records[selected].client_ip = ip;
        s_ipv4_records[selected].is_active = true;
    }

    s_ipv4_records[selected].client_port = port;
    s_ipv4_records[selected].prev_t2 = t2;
    s_ipv4_records[selected].prev_t3 = t3;
    s_ipv4_records[selected].last_seen_ms = current_time_ms();
    xSemaphoreGive(s_mutex);
    return true;
}

bool ntp_cache_find_or_create_ipv6(const struct in6_addr *ip, uint16_t port, ntp_client_record_ipv6_t *record,
                                   bool *rate_limited)
{
    if (rate_limited != NULL)
    {
        *rate_limited = false;
    }
    if (s_mutex == NULL || ip == NULL || record == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    bool found = false;
    const size_t selected = select_ipv6_slot(ip, port, &found);

    if (!found)
    {
        memset(&s_ipv6_records[selected], 0, sizeof(s_ipv6_records[selected]));
        s_ipv6_records[selected].client_ip = *ip;
        s_ipv6_records[selected].is_active = true;
        s_ipv6_records[selected].last_seen_ms = current_time_ms();
    }
    s_ipv6_records[selected].client_port = port;
    if (rate_limited != NULL)
    {
        *rate_limited = rate_limit_exceeded(&s_ipv6_records[selected].rate_tat_us);
    }

    *record = s_ipv6_records[selected];
    xSemaphoreGive(s_mutex);
    return true;
}

bool ntp_cache_update_ipv6(const struct in6_addr *ip, uint16_t port, uint64_t t2, uint64_t t3)
{
    if (s_mutex == NULL || ip == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    bool found = false;
    const size_t selected = select_ipv6_slot(ip, port, &found);

    if (!found)
    {
        memset(&s_ipv6_records[selected], 0, sizeof(s_ipv6_records[selected]));
        s_ipv6_records[selected].client_ip = *ip;
        s_ipv6_records[selected].is_active = true;
    }

    s_ipv6_records[selected].client_port = port;
    s_ipv6_records[selected].prev_t2 = t2;
    s_ipv6_records[selected].prev_t3 = t3;
    s_ipv6_records[selected].last_seen_ms = current_time_ms();
    xSemaphoreGive(s_mutex);
    return true;
}

void ntp_cache_purge_expired(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    const int64_t now_ms = current_time_ms();

    for (size_t i = 0; i < MAX_TRACKED_CLIENTS_IPV4; ++i)
    {
        if (s_ipv4_records[i].is_active && now_ms - s_ipv4_records[i].last_seen_ms > CACHE_TIMEOUT_MS_IPV4)
        {
            memset(&s_ipv4_records[i], 0, sizeof(s_ipv4_records[i]));
        }
    }
    for (size_t i = 0; i < MAX_TRACKED_CLIENTS_IPV6; ++i)
    {
        if (s_ipv6_records[i].is_active && now_ms - s_ipv6_records[i].last_seen_ms > CACHE_TIMEOUT_MS_IPV6)
        {
            memset(&s_ipv6_records[i], 0, sizeof(s_ipv6_records[i]));
        }
    }
    xSemaphoreGive(s_mutex);
}
