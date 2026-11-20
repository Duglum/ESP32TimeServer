// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "web_tls_pair.h"

#include <cstring>

namespace
{
    constexpr size_t ENTRY_SIZE = 32;
    constexpr size_t CHUNK_MAX = 4000; // ENTRY_SIZE * (126 - 1), Page::CHUNK_MAX_SIZE of ESP-IDF
} // namespace

size_t web_tls_pair_pack(const char *cert, const char *key, uint8_t *out, size_t out_size)
{
    if (cert == nullptr || key == nullptr || cert[0] == '\0' || key[0] == '\0')
        return 0;
    const size_t cert_length = strlen(cert);
    const size_t key_length = strlen(key);
    const size_t size = web_tls_pair_size(cert_length, key_length);
    if (out == nullptr || size > out_size)
        return 0;
    memcpy(out, cert, cert_length + 1);
    memcpy(out + cert_length + 1, key, key_length + 1);
    return size;
}

bool web_tls_pair_split(const uint8_t *data, size_t size, const char *&cert, const char *&key)
{
    cert = key = nullptr;
    if (data == nullptr || size < 4 || data[size - 1] != '\0')
        return false;
    const void *end = memchr(data, '\0', size);
    const size_t cert_length = static_cast<const uint8_t *>(end) - data;
    // A chain of at least one character, then a key of at least one character that ends with the last byte
    if (cert_length == 0 || cert_length + 2 >= size || strlen(reinterpret_cast<const char *>(data) + cert_length + 1) != size - cert_length - 2)
        return false;
    cert = reinterpret_cast<const char *>(data);
    key = reinterpret_cast<const char *>(data) + cert_length + 1;
    return true;
}

size_t web_tls_nvs_entries_max(size_t size)
{
    const size_t chunks = (size + CHUNK_MAX - 1) / CHUNK_MAX + 1;
    // Data rounded up per chunk: at most one entry more per chunk than the whole size rounded up
    return (size + ENTRY_SIZE - 1) / ENTRY_SIZE + (chunks - 1) + chunks + 1;
}

size_t web_tls_nvs_entries_min(size_t size)
{
    return (size + ENTRY_SIZE - 1) / ENTRY_SIZE + 2;
}

bool web_tls_nvs_room(size_t available, size_t new_size, size_t extra_entries, size_t old_entries_min, size_t reserve, size_t &needed)
{
    const size_t entries = web_tls_nvs_entries_max(new_size) + extra_entries;
    needed = entries;
    // Afterwards the old data is erased: a larger blob also needs the reserve
    if (entries > old_entries_min && entries - old_entries_min + reserve > needed)
        needed = entries - old_entries_min + reserve;
    return available >= needed;
}

namespace
{
    // The whole item a half of the former layout is equal to (bit 1: opair2, bit 0: opair; opair2 first)
    web_tls_own_source_t whole(uint8_t in)
    {
        return (in & WEB_TLS_OWN_IN_OPAIR2) != 0 ? web_tls_own_source_t::opair2 : web_tls_own_source_t::opair;
    }

    // The rule on which items exist
    web_tls_own_source_t by_presence(const web_tls_own_items_t &items)
    {
        switch (items.okey)
        {
        case web_tls_okey_t::key:
            return items.ocrt ? web_tls_own_source_t::former : web_tls_own_source_t::none;
        case web_tls_okey_t::marker:
            if (items.opair)
                return web_tls_own_source_t::opair;
            return items.opair2 ? web_tls_own_source_t::opair2 : web_tls_own_source_t::none;
        case web_tls_okey_t::absent:
        default:
            return items.opair && items.mode ? web_tls_own_source_t::opair : web_tls_own_source_t::none;
        }
    }

    void add(web_tls_own_plan_t &plan, web_tls_own_step_t step)
    {
        if (plan.count < sizeof(plan.steps) / sizeof(plan.steps[0]))
            plan.steps[plan.count++] = step;
    }

    void append(web_tls_own_plan_t &plan, const web_tls_own_plan_t &more)
    {
        for (uint8_t i = 0; i < more.count; ++i)
            add(plan, more.steps[i]);
    }
} // namespace

web_tls_own_source_t web_tls_own_decide(const web_tls_own_items_t &items)
{
    const web_tls_own_source_t source = by_presence(items);
    const uint8_t ocrt_in = items.ocrt ? items.ocrt_in : 0;
    const uint8_t okey_in = items.okey == web_tls_okey_t::key ? items.okey_in : 0;
    // A half of the former layout that is equal to a half of a whole item: the firmware before 478c2ff stored that
    // pair again and its store was cut off or failed halfway (or a reset or a later erase was), and the pair is still
    // whole in that item. Chain first: the firmware before 478c2ff writes it after the key of its store before.
    if (items.okey == web_tls_okey_t::key)
    {
        if ((ocrt_in & okey_in) != 0)
            return web_tls_own_source_t::former; // both halves of the same pair
        if (ocrt_in != 0)
            return whole(ocrt_in);
        if (okey_in != 0)
            return whole(okey_in);
        return source;
    }
    if (items.okey == web_tls_okey_t::absent && ocrt_in != 0)
        return whole(ocrt_in);
    return source;
}

void web_tls_own_apply(web_tls_own_items_t &items, web_tls_own_step_t step)
{
    switch (step)
    {
    case web_tls_own_step_t::erase_ocrt:
        items.ocrt = false;
        items.ocrt_in = 0;
        break;
    case web_tls_own_step_t::erase_okey:
        items.okey = web_tls_okey_t::absent;
        items.okey_in = 0;
        break;
    case web_tls_own_step_t::erase_opair:
        items.opair = false;
        items.ocrt_in &= static_cast<uint8_t>(~WEB_TLS_OWN_IN_OPAIR);
        items.okey_in &= static_cast<uint8_t>(~WEB_TLS_OWN_IN_OPAIR);
        break;
    case web_tls_own_step_t::erase_opair2:
        items.opair2 = false;
        items.ocrt_in &= static_cast<uint8_t>(~WEB_TLS_OWN_IN_OPAIR2);
        items.okey_in &= static_cast<uint8_t>(~WEB_TLS_OWN_IN_OPAIR2);
        break;
    case web_tls_own_step_t::write_marker:
        items.okey = web_tls_okey_t::marker;
        items.okey_in = 0;
        break;
    case web_tls_own_step_t::copy_opair:
        items.opair2 = true;
        items.ocrt_in = static_cast<uint8_t>((items.ocrt_in & WEB_TLS_OWN_IN_OPAIR) * 3);
        items.okey_in = static_cast<uint8_t>((items.okey_in & WEB_TLS_OWN_IN_OPAIR) * 3);
        break;
    case web_tls_own_step_t::write_new:
        items.opair2 = true;
        items.ocrt_in = static_cast<uint8_t>((items.ocrt_in & ~WEB_TLS_OWN_IN_OPAIR2) | ((items.new_in & 1) != 0 ? WEB_TLS_OWN_IN_OPAIR2 : 0));
        items.okey_in = static_cast<uint8_t>((items.okey_in & ~WEB_TLS_OWN_IN_OPAIR2) | ((items.new_in & 2) != 0 ? WEB_TLS_OWN_IN_OPAIR2 : 0));
        break;
    }
}

web_tls_own_plan_t web_tls_own_start(const web_tls_own_items_t &items)
{
    web_tls_own_plan_t plan;
    plan.source = web_tls_own_decide(items);
    switch (plan.source)
    {
    case web_tls_own_source_t::former:
        // Older than the former pair (or equal to it); erasing them keeps the former pair
        if (items.opair2)
            add(plan, web_tls_own_step_t::erase_opair2);
        if (items.opair)
            add(plan, web_tls_own_step_t::erase_opair);
        break;
    case web_tls_own_source_t::opair:
        // Moved into opair2: copied first, then the marker (with it opair decides), then opair and ocrt erased
        if (items.opair2)
            add(plan, web_tls_own_step_t::erase_opair2); // older than opair (or equal to it), and room for the copy
        add(plan, web_tls_own_step_t::copy_opair);
        if (items.okey != web_tls_okey_t::marker)
            add(plan, web_tls_own_step_t::write_marker);
        add(plan, web_tls_own_step_t::erase_opair);
        if (items.ocrt)
            add(plan, web_tls_own_step_t::erase_ocrt);
        break;
    case web_tls_own_source_t::opair2:
        if (items.opair)
            add(plan, web_tls_own_step_t::erase_opair); // older (opair2 decides only while opair is not newer)
        if (items.okey != web_tls_okey_t::marker)
            add(plan, web_tls_own_step_t::write_marker);
        if (items.ocrt)
            add(plan, web_tls_own_step_t::erase_ocrt); // the first write of a store of the firmware before
        break;
    case web_tls_own_source_t::none:
    default:
        // Pairs that no longer count; a key in okey last (while it is there, opair and opair2 do not count)
        if (items.opair2)
            add(plan, web_tls_own_step_t::erase_opair2);
        if (items.opair)
            add(plan, web_tls_own_step_t::erase_opair);
        if (items.okey == web_tls_okey_t::key)
            add(plan, web_tls_own_step_t::erase_okey);
        break;
    }
    plan.commit = plan.count;
    return plan;
}

web_tls_own_plan_t web_tls_own_store(const web_tls_own_items_t &items)
{
    // The steps of a start first (each keeps the pair stored now), then from that state: the new pair into opair2,
    // the marker, ocrt erased
    web_tls_own_plan_t plan = web_tls_own_start(items);
    web_tls_own_items_t after = items;
    for (uint8_t i = 0; i < plan.count; ++i)
        web_tls_own_apply(after, plan.steps[i]);
    web_tls_own_plan_t rest;
    add(rest, web_tls_own_step_t::write_new);
    if (after.okey != web_tls_okey_t::marker)
        add(rest, web_tls_own_step_t::write_marker);
    if (after.ocrt)
        add(rest, web_tls_own_step_t::erase_ocrt);
    append(plan, rest);
    // The commit: the first step after which the rule takes the new pair
    plan.commit = plan.count;
    after = items;
    bool written = false;
    for (uint8_t i = 0; i < plan.count; ++i)
    {
        web_tls_own_apply(after, plan.steps[i]);
        written = written || plan.steps[i] == web_tls_own_step_t::write_new;
        // opair2, or the former items once they are the halves of the new pair
        const web_tls_own_source_t now = web_tls_own_decide(after);
        if (written && (now == web_tls_own_source_t::opair2 || (now == web_tls_own_source_t::former && (after.ocrt_in & after.okey_in & WEB_TLS_OWN_IN_OPAIR2) != 0)))
        {
            plan.commit = i;
            break;
        }
    }
    return plan;
}

web_tls_own_plan_t web_tls_own_clear(const web_tls_own_items_t &items)
{
    web_tls_own_plan_t plan;
    plan.source = web_tls_own_decide(items);
    // The whole item that does not decide first (while the one that decides is there, the other one does not count),
    // then the former pair as the firmware before 478c2ff erases it, the marker last
    const bool opair_first = plan.source != web_tls_own_source_t::opair;
    if (opair_first && items.opair)
        add(plan, web_tls_own_step_t::erase_opair);
    if (items.opair2)
        add(plan, web_tls_own_step_t::erase_opair2);
    if (!opair_first && items.opair)
        add(plan, web_tls_own_step_t::erase_opair);
    if (items.ocrt)
        add(plan, web_tls_own_step_t::erase_ocrt);
    if (items.okey != web_tls_okey_t::absent)
        add(plan, web_tls_own_step_t::erase_okey);
    plan.commit = plan.count;
    return plan;
}
