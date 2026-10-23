// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lwip/inet.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_TRACKED_CLIENTS_IPV4 128
#define CACHE_TIMEOUT_MS_IPV4 1800000
#define MAX_TRACKED_CLIENTS_IPV6 128
#define CACHE_TIMEOUT_MS_IPV6 1800000

    bool ntp_cache_init(void);
    void ntp_cache_deinit(void);
    // Per-client rate limit in requests per second (0 = off), takes effect at once. Can be called before ntp_cache_init().
    void ntp_cache_set_rate_limit(uint32_t requests_per_second);
    // Finds (or creates) the record of the client address and the interleaved state of the request.
    // origin_timestamp: origin timestamp of the request (0 = none). *interleaved_t3 (may be NULL) is set to the T3 of
    // the earlier exchange with this address whose T2 equals the origin timestamp, 0 if there is none (basic mode).
    // rate_limited: NULL = no rate limit check (requests of this device to itself), otherwise set to true if the
    // request exceeds the per-client rate limit (it must then be dropped without an answer)
    bool ntp_cache_find_or_create(uint32_t ip, uint16_t port, uint64_t origin_timestamp, uint64_t *interleaved_t3,
                                  bool *rate_limited);
    // Stores the exchange of an answered request (t2 = 0: none, the next request of the client gets a basic reply)
    bool ntp_cache_update(uint32_t ip, uint16_t port, uint64_t t2, uint64_t t3);
    bool ntp_cache_find_or_create_ipv6(const struct in6_addr *ip, uint16_t port, uint64_t origin_timestamp,
                                       uint64_t *interleaved_t3, bool *rate_limited);
    bool ntp_cache_update_ipv6(const struct in6_addr *ip, uint16_t port, uint64_t t2, uint64_t t3);
    void ntp_cache_purge_expired(void);

#ifdef __cplusplus
}
#endif
