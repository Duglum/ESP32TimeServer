// ESP32 Time Server - host tests
// NTP client cache (main/custom/ntp_cache.c): interleaved state per address, exchange map, per-client rate limit,
// IPv6 rate limit per /64, expiry. Built twice by run_tests.ps1: as a test program (main) and, with HOST_FUZZ, as a
// libFuzzer target that checks every interleaved T3 against a model of the stored exchanges.

#include "ntp_cache.h"
#include "host_hooks.h"
#include "host_stubs.h"

#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace
{
    uint64_t s_next_t2 = 0x1234567800000000ULL; // distinct T2 values per test (the exchange map never expires)

    uint64_t next_t2()
    {
        s_next_t2 += 0x0000000100000000ULL + 0x9E37; // about 1 s apart, low bits not round
        return s_next_t2;
    }

    struct in6_addr ipv6(uint64_t prefix, uint64_t host)
    {
        struct in6_addr address{};
        for (int i = 0; i < 8; ++i)
        {
            address.un.u8_addr[i] = static_cast<uint8_t>(prefix >> (56 - 8 * i));
            address.un.u8_addr[8 + i] = static_cast<uint8_t>(host >> (56 - 8 * i));
        }
        return address;
    }

    // A request: returns the interleaved T3 (0 = basic mode), *limited the rate limit decision
    uint64_t request4(uint32_t ip, uint64_t origin, bool *limited = nullptr, uint16_t *slot = nullptr)
    {
        uint64_t t3 = 0;
        bool rate_limited = false;
        CHECK(ntp_cache_find_or_create(ip, origin, &t3, &rate_limited, slot));
        if (limited != nullptr)
            *limited = rate_limited;
        return t3;
    }

    uint64_t request6(const struct in6_addr &ip, uint64_t origin, bool per_prefix, bool *limited = nullptr)
    {
        uint64_t t3 = 0;
        bool rate_limited = false;
        uint16_t slot = NTP_CACHE_SLOT_NONE;
        CHECK(ntp_cache_find_or_create_ipv6(&ip, origin, &t3, &rate_limited, &slot, per_prefix));
        if (limited != nullptr)
            *limited = rate_limited;
        return t3;
    }

    // Forgets all client records (the exchange map stays)
    void expire_all()
    {
        host_now_us += (CACHE_TIMEOUT_MS_IPV4 + 1000LL) * 1000LL;
        ntp_cache_purge_expired();
    }

    void test_interleaved_ipv4()
    {
        ntp_cache_set_rate_limit(0);
        expire_all();
        const uint32_t client = 0xC0A80810;
        const uint64_t t2 = next_t2();
        const uint64_t t3 = t2 + 0x00010000; // 15 us later
        uint16_t slot = NTP_CACHE_SLOT_NONE;
        CHECK_EQ(request4(client, 0, nullptr, &slot), 0); // first request: basic mode
        CHECK(slot != NTP_CACHE_SLOT_NONE);
        CHECK(ntp_cache_update(client, slot, t2, t3));
        CHECK_EQ(request4(client, t2), t3);         // latest exchange of the record
        CHECK_EQ(request4(client, t2 + 1), 0);      // unknown origin
        CHECK_EQ(request4(0xC0A80811, t2), 0);      // another address with the same origin

        // Another host with the same address replaces the record; the exchange map keeps the client's T2
        const uint64_t other_t2 = next_t2();
        CHECK(ntp_cache_update(client, NTP_CACHE_SLOT_NONE, other_t2, other_t2 + 0x00010000));
        CHECK_EQ(request4(client, t2), t3);
        CHECK_EQ(request4(client, other_t2), other_t2 + 0x00010000);

        // A T3 that is not after T2 within a second is kept in the record only
        const uint64_t bad_t2 = next_t2();
        CHECK(ntp_cache_update(client, NTP_CACHE_SLOT_NONE, bad_t2, bad_t2 - 1));
        CHECK(ntp_cache_update(client, NTP_CACHE_SLOT_NONE, next_t2(), 0));
        CHECK_EQ(request4(client, bad_t2), 0);

        // The record expires after 30 minutes; the map entry stays
        expire_all();
        CHECK_EQ(request4(client, t2), t3);
        ntp_cache_stats_t stats{};
        ntp_cache_get_stats(&stats);
        CHECK(stats.record_hits >= 1 && stats.map_hits >= 2 && stats.misses >= 2);
    }

    void test_stale_slot()
    {
        ntp_cache_set_rate_limit(0);
        expire_all();
        // Fill all records: the slot of the first client goes to a new client, the update must not use it blindly
        uint16_t first_slot = NTP_CACHE_SLOT_NONE;
        (void)request4(0x0A000001, 0, nullptr, &first_slot);
        for (uint32_t i = 2; i <= MAX_TRACKED_CLIENTS_IPV4 + 1; ++i)
        {
            host_now_us += 1000;
            (void)request4(0x0A000000 + i, 0);
        }
        const uint64_t t2 = next_t2();
        CHECK(ntp_cache_update(0x0A000001, first_slot, t2, t2 + 5));
        // The client that got the slot keeps its own state; the first client is found again by its address
        CHECK_EQ(request4(0x0A000001, t2), t2 + 5);
        uint64_t t3 = 0;
        CHECK(ntp_cache_find_or_create(0x0A000000 + MAX_TRACKED_CLIENTS_IPV4 + 1, t2, &t3, nullptr, nullptr));
        CHECK_EQ(t3, 0);
    }

    void test_rate_limit_ipv4()
    {
        expire_all();
        ntp_cache_set_rate_limit(4); // interval 250 ms, burst one second
        const uint32_t client = 0xC0A80820;
        bool limited = false;
        for (int i = 0; i < 4; ++i)
        {
            (void)request4(client, 0, &limited);
            CHECK(!limited);
        }
        (void)request4(client, 0, &limited);
        CHECK(limited);
        host_now_us += 250000;
        (void)request4(client, 0, &limited);
        CHECK(!limited);
        (void)request4(client, 0, &limited);
        CHECK(limited);

        // Another address has its own limit
        (void)request4(client + 1, 0, &limited);
        CHECK(!limited);

        // Requests with the client's T2 (interleaved) have their own bucket: still answered
        const uint64_t t2 = next_t2();
        CHECK(ntp_cache_update(client, NTP_CACHE_SLOT_NONE, t2, t2 + 7));
        CHECK_EQ(request4(client, t2, &limited), t2 + 7);
        CHECK(!limited);
        // A limited request gets no interleaved T3
        for (int i = 0; i < 4; ++i)
            (void)request4(client, t2, &limited);
        CHECK(limited);
        CHECK_EQ(request4(client, t2, &limited), 0);

        // Sustained rate: 4 per second are answered over 10 s
        host_now_us += 2000000;
        int answered = 0;
        for (int i = 0; i < 80; ++i)
        {
            host_now_us += 125000;
            (void)request4(client + 2, 0, &limited);
            answered += limited ? 0 : 1;
        }
        CHECK(answered >= 40 && answered <= 44);

        // No check for requests of the device itself, rate limit off
        uint64_t t3 = 0;
        for (int i = 0; i < 20; ++i)
            CHECK(ntp_cache_find_or_create(client, 0, &t3, nullptr, nullptr));
        ntp_cache_set_rate_limit(0);
        for (int i = 0; i < 20; ++i)
        {
            (void)request4(client, 0, &limited);
            CHECK(!limited);
        }
    }

    void test_ipv6_prefix_rate_limit()
    {
        expire_all();
        ntp_cache_set_rate_limit(2);
        const uint64_t remote = 0x20010db800010000ULL;
        bool limited = false;
        // Per /64: two addresses of the same remote /64 share the limit of 2 per second
        (void)request6(ipv6(remote, 1), 0, true, &limited);
        CHECK(!limited);
        (void)request6(ipv6(remote, 2), 0, true, &limited);
        CHECK(!limited);
        (void)request6(ipv6(remote, 3), 0, true, &limited);
        CHECK(limited);
        // Another /64 has its own limit
        (void)request6(ipv6(remote + 1, 1), 0, true, &limited);
        CHECK(!limited);
        // Per address (own prefix): each address its own limit
        const uint64_t local = 0xfd00000000000001ULL;
        for (uint64_t host = 1; host <= 4; ++host)
        {
            (void)request6(ipv6(local, host), 0, false, &limited);
            CHECK(!limited);
        }

        // The interleaved state stays per address, also with the limit per /64
        ntp_cache_set_rate_limit(0);
        const uint64_t t2 = next_t2();
        const struct in6_addr a = ipv6(remote, 1);
        const struct in6_addr b = ipv6(remote, 2);
        CHECK(ntp_cache_update_ipv6(&a, NTP_CACHE_SLOT_NONE, t2, t2 + 3));
        CHECK_EQ(request6(a, t2, true), t2 + 3);
        CHECK_EQ(request6(b, t2, true), 0);
        // Expiry of records and prefixes
        expire_all();
        ntp_cache_set_rate_limit(2);
        (void)request6(ipv6(remote, 9), 0, true, &limited);
        CHECK(!limited);
        CHECK_EQ(request6(a, t2, true), t2 + 3); // from the exchange map
        ntp_cache_set_rate_limit(0);
    }

    void test_many_prefixes()
    {
        // More /64s than prefix records: the least recently used one is replaced, nothing breaks
        expire_all();
        ntp_cache_set_rate_limit(1);
        bool limited = false;
        for (uint64_t p = 0; p < 3 * MAX_TRACKED_PREFIXES_IPV6; ++p)
        {
            host_now_us += 1000;
            (void)request6(ipv6(0x2001000000000000ULL + p, 1), 0, true, &limited);
            CHECK(!limited);
        }
        ntp_cache_set_rate_limit(0);
    }
}

#ifndef HOST_FUZZ
int main()
{
    CHECK(ntp_cache_init());
    test_interleaved_ipv4();
    test_stale_slot();
    test_rate_limit_ipv4();
    test_ipv6_prefix_rate_limit();
    test_many_prefixes();
    ntp_cache_deinit();
    std::printf("ntp_cache_test: %d checks, %d failed\n", host_checks, host_failures);
    return host_failures == 0 ? 0 : 1;
}
#else
// Fuzzing: a sequence of operations on a few addresses. Every interleaved T3 the cache returns must be a T3 that was
// stored with this T2 for this address (it must never hand out the T3 of another client or a wrong one).
namespace
{
    std::map<std::pair<uint32_t, uint64_t>, std::set<uint64_t>> s_model; // (address index, T2) -> T3 stored

    uint64_t take(const uint8_t *&data, size_t &size, size_t bytes)
    {
        uint64_t value = 0;
        for (size_t i = 0; i < bytes && size > 0; ++i, ++data, --size)
            value = (value << 8) | *data;
        return value;
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool initialised = ntp_cache_init();
    (void)initialised;
    // A clean cache: all records expired; the exchange map keeps the entries of earlier inputs, so each input uses
    // T2 values of its own
    expire_all();
    s_model.clear();
    ntp_cache_set_rate_limit(0);
    static uint64_t s_input = 0;
    const uint64_t t2_base = 0x8000000000000000ULL + (++s_input << 32);

    while (size >= 2)
    {
        const uint8_t op = static_cast<uint8_t>(take(data, size, 1));
        const uint32_t index = op & 0x0F;                  // 16 addresses: 8 IPv4, 8 IPv6 (4 in one /64)
        const bool v6 = index >= 8;
        const uint32_t ip4 = 0x0A000000u + index;
        const struct in6_addr ip6 = ipv6(index < 12 ? 0x20010db8aaaa0000ULL : 0x20010db8bbbb0000ULL + index, index);
        // T2 values from a small set, so that the fuzzer can hit them again
        const uint64_t t2 = t2_base + (take(data, size, 1) << 20);
        switch (op >> 4)
        {
        case 0: // request with an origin timestamp
        case 1:
        case 2:
        case 3:
        {
            uint64_t t3 = 0;
            bool limited = false;
            uint16_t slot = NTP_CACHE_SLOT_NONE;
            bool *limit = (op >> 4) == 3 ? nullptr : &limited;
            const bool ok = v6 ? ntp_cache_find_or_create_ipv6(&ip6, t2, &t3, limit, &slot, (op >> 4) == 1)
                               : ntp_cache_find_or_create(ip4, t2, &t3, limit, &slot);
            if (!ok || slot == NTP_CACHE_SLOT_NONE)
                __builtin_trap();
            if (t3 != 0)
            {
                const auto it = s_model.find({index, t2});
                if (it == s_model.end() || it->second.count(t3) == 0)
                    __builtin_trap(); // a T3 that was never stored for this address and T2
            }
            break;
        }
        case 4: // answered request: the exchange is stored
        case 5:
        case 6:
        {
            const uint64_t t3 = t2 + take(data, size, 3) * (((op >> 4) == 6) ? 0x10000 : 1); // sometimes > 1 s
            const uint16_t slot = static_cast<uint16_t>(take(data, size, 1));
            const bool ok = v6 ? ntp_cache_update_ipv6(&ip6, slot, t2, t3) : ntp_cache_update(ip4, slot, t2, t3);
            if (!ok)
                __builtin_trap();
            s_model[{index, t2}].insert(t3);
            break;
        }
        case 7:
            host_now_us += static_cast<int64_t>(take(data, size, 2)) * 1000;
            break;
        case 8:
            host_now_us += static_cast<int64_t>(take(data, size, 1)) * 60000000LL;
            ntp_cache_purge_expired();
            break;
        case 9:
            ntp_cache_set_rate_limit(static_cast<uint32_t>(take(data, size, 1)));
            break;
        default: // other addresses (fill the tables)
        {
            const uint32_t other = static_cast<uint32_t>(take(data, size, 2));
            uint64_t t3 = 0;
            bool limited = false;
            if (v6)
            {
                const struct in6_addr address = ipv6(0x3000000000000000ULL + other, other);
                (void)ntp_cache_find_or_create_ipv6(&address, t2, &t3, &limited, nullptr, true);
                if (t3 != 0)
                    __builtin_trap(); // never stored for these addresses
            }
            else
            {
                (void)ntp_cache_find_or_create(0x0B000000u + other, t2, &t3, &limited, nullptr);
                if (t3 != 0)
                    __builtin_trap();
            }
            break;
        }
        }
    }
    return 0;
}
#endif
