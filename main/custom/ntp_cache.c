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

// ---- Exchange map (interleaved mode, RFC 9769) ----
// A client in interleaved mode sends the T2 of the last reply it received back as the origin timestamp of its next
// request, and the reply carries the T3 of that exchange. The client record keeps the latest exchange of its address
// (for 30 minutes, also for clients that poll rarely). This map keeps the recent exchanges of all clients, found by
// their T2: a request with a spoofed source address, or a request of another client behind the same NAT address,
// replaces the latest exchange of the address record, but not the map entry of the real client, which therefore keeps
// its interleaved mode. T2 is a receive time stamp with ns resolution that an off-path sender cannot know (it works
// as a cookie); in addition the entry has to belong to the same source address.
// 4-way set associative: the set is chosen by a hash of T2, a new exchange replaces the oldest entry of its set.
// 512 sets x 4 ways x 16 bytes = 32 kB. An entry survives 500 later exchanges (of all clients) almost surely, 1000
// with 87 % and 2000 with 40 % probability (simulation), so a spoofing source has to get about 1000 to 2000 requests
// answered per poll interval of its victim to push the victim into basic mode (before: one request).
#define EXCHANGE_MAP_SET_BITS 9
#define EXCHANGE_MAP_SETS (1u << EXCHANGE_MAP_SET_BITS)
#define EXCHANGE_MAP_WAYS 4

typedef struct
{
    uint32_t client_ip;
    uint16_t client_port;
    uint64_t prev_t2; // latest exchange of this address (0 = none)
    uint64_t prev_t3;
    int64_t last_seen_ms;
    int64_t rate_tat_us;        // per-client rate limit: theoretical arrival time of the next request (esp_timer)
    int64_t rate_tat_cookie_us; // the same for requests with a T2 cookie (see check_request_locked)
    bool is_active;
} ntp_client_record_t;

typedef struct
{
    struct in6_addr client_ip;
    uint16_t client_port;
    uint64_t prev_t2; // latest exchange of this address (0 = none)
    uint64_t prev_t3;
    int64_t last_seen_ms;
    int64_t rate_tat_us;        // per-client rate limit: theoretical arrival time of the next request (esp_timer)
    int64_t rate_tat_cookie_us; // the same for requests with a T2 cookie (see check_request_locked)
    bool is_active;
} ntp_client_record_ipv6_t;

typedef struct
{
    uint64_t t2;          // T2 of the exchange (NTP 32.32 format), 0 = free
    uint32_t t3_offset;   // T3 - T2 (NTP 32.32 format, below 1 s)
    uint32_t address_tag; // client address (see address_tag_ipv4 / address_tag_ipv6)
} exchange_entry_t;

static ntp_client_record_t s_ipv4_records[MAX_TRACKED_CLIENTS_IPV4];
static ntp_client_record_ipv6_t s_ipv6_records[MAX_TRACKED_CLIENTS_IPV6];
static exchange_entry_t s_exchanges[EXCHANGE_MAP_SETS][EXCHANGE_MAP_WAYS];
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

static uint32_t address_tag_ipv4(uint32_t ip)
{
    return ip;
}

// FNV-1a over the 16 bytes of the address
static uint32_t address_tag_ipv6(const struct in6_addr *ip)
{
    const uint8_t *bytes = (const uint8_t *)ip;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < sizeof(*ip); ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

// Set of a T2 in the exchange map. Fibonacci hashing: the low bits of T2 alone are not evenly spread (the time
// stamps advance in steps of 13 ns).
static size_t exchange_set(uint64_t t2)
{
    return (size_t)((t2 * 0x9E3779B97F4A7C15ULL) >> (64 - EXCHANGE_MAP_SET_BITS));
}

// Must be called with s_mutex held.
static void exchange_store(uint32_t address_tag, uint64_t t2, uint64_t t3)
{
    // T3 - T2 in modulo arithmetic (also right when T3 is already in the next NTP era); a T3 before T2 gives a huge value
    const uint64_t t3_offset = t3 - t2;
    if (t2 == 0 || t3_offset > UINT32_MAX)
    {
        return; // no exchange to store (no T2, or not a plausible T3)
    }
    exchange_entry_t *set = s_exchanges[exchange_set(t2)];
    exchange_entry_t *victim = &set[0];
    int64_t victim_age = INT64_MIN;
    for (size_t way = 0; way < EXCHANGE_MAP_WAYS; ++way)
    {
        if (set[way].t2 == t2)
        {
            victim = &set[way];
            break;
        }
        // Age against the new T2 (the difference also works across the NTP era change); a free entry is the oldest
        const int64_t age = set[way].t2 == 0 ? INT64_MAX : (int64_t)(t2 - set[way].t2);
        if (age > victim_age)
        {
            victim_age = age;
            victim = &set[way];
        }
    }
    victim->t2 = t2;
    victim->t3_offset = (uint32_t)t3_offset;
    victim->address_tag = address_tag;
}

// Must be called with s_mutex held.
// T3 of the earlier exchange with this address whose T2 is the origin timestamp of the request, 0 if there is none.
static uint64_t interleaved_t3_locked(uint64_t record_t2, uint64_t record_t3, uint32_t address_tag, uint64_t origin_timestamp)
{
    if (origin_timestamp == 0)
    {
        return 0;
    }
    if (record_t2 == origin_timestamp)
    {
        return record_t3; // the latest exchange of this address
    }
    const exchange_entry_t *set = s_exchanges[exchange_set(origin_timestamp)];
    for (size_t way = 0; way < EXCHANGE_MAP_WAYS; ++way)
    {
        if (set[way].t2 == origin_timestamp && set[way].address_tag == address_tag)
        {
            return origin_timestamp + set[way].t3_offset;
        }
    }
    return 0;
}

// Must be called with s_mutex held.
// Interleaved state and rate limit of a request. The interleaved lookup comes first: a request whose origin timestamp
// is the T2 of an earlier exchange with this address carries a value that only the real client got (in the reply to
// that exchange, see the exchange map), so it is counted in a bucket of its own with the same limit. A host sending
// with the client's address but without its T2 fills the other bucket and can no longer exhaust the limit of the
// client's interleaved requests (review S-17).
static void check_request_locked(int64_t *rate_tat_us, int64_t *rate_tat_cookie_us, uint64_t record_t2,
                                 uint64_t record_t3, uint32_t address_tag, uint64_t origin_timestamp,
                                 uint64_t *interleaved_t3, bool *rate_limited)
{
    const uint64_t t3 = interleaved_t3_locked(record_t2, record_t3, address_tag, origin_timestamp);
    const bool limited = rate_limited != NULL && rate_limit_exceeded(t3 != 0 ? rate_tat_cookie_us : rate_tat_us);
    if (rate_limited != NULL)
    {
        *rate_limited = limited;
    }
    if (interleaved_t3 != NULL && !limited)
    {
        *interleaved_t3 = t3;
    }
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

bool ntp_cache_find_or_create(uint32_t ip, uint16_t port, uint64_t origin_timestamp, uint64_t *interleaved_t3,
                              bool *rate_limited)
{
    if (rate_limited != NULL)
    {
        *rate_limited = false;
    }
    if (interleaved_t3 != NULL)
    {
        *interleaved_t3 = 0;
    }
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
        s_ipv4_records[selected].last_seen_ms = current_time_ms();
    }
    s_ipv4_records[selected].client_port = port;
    check_request_locked(&s_ipv4_records[selected].rate_tat_us, &s_ipv4_records[selected].rate_tat_cookie_us,
                         s_ipv4_records[selected].prev_t2, s_ipv4_records[selected].prev_t3, address_tag_ipv4(ip),
                         origin_timestamp, interleaved_t3, rate_limited);

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
    exchange_store(address_tag_ipv4(ip), t2, t3);
    xSemaphoreGive(s_mutex);
    return true;
}

bool ntp_cache_find_or_create_ipv6(const struct in6_addr *ip, uint16_t port, uint64_t origin_timestamp,
                                   uint64_t *interleaved_t3, bool *rate_limited)
{
    if (rate_limited != NULL)
    {
        *rate_limited = false;
    }
    if (interleaved_t3 != NULL)
    {
        *interleaved_t3 = 0;
    }
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
        s_ipv6_records[selected].last_seen_ms = current_time_ms();
    }
    s_ipv6_records[selected].client_port = port;
    check_request_locked(&s_ipv6_records[selected].rate_tat_us, &s_ipv6_records[selected].rate_tat_cookie_us,
                         s_ipv6_records[selected].prev_t2, s_ipv6_records[selected].prev_t3, address_tag_ipv6(ip),
                         origin_timestamp, interleaved_t3, rate_limited);

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
    exchange_store(address_tag_ipv6(ip), t2, t3);
    xSemaphoreGive(s_mutex);
    return true;
}

// Expires the client records. The exchange map needs no expiry: an entry stays a correct pair of T2 and T3 at any
// age, and newer exchanges replace it.
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
