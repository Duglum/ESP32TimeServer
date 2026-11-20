// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The pure part of the presence marks (see record_mark.h). Host tested.

#include "sys/record_mark.h"

#include <cstring>

record_mark_state_t record_mark_decide(bool marked, bool record_valid)
{
    if (record_valid)
        return marked ? record_mark_state_t::present : record_mark_state_t::adopted;
    return marked ? record_mark_state_t::lost : record_mark_state_t::none;
}

bool record_mark_key(const char *ns, const char *key, char (&out)[RECORD_MARK_KEY_SIZE])
{
    out[0] = '\0';
    if (ns == nullptr || key == nullptr || ns[0] == '\0' || key[0] == '\0')
        return false;
    const size_t ns_length = strlen(ns);
    const size_t key_length = strlen(key);
    if (ns_length + 1 + key_length > RECORD_MARK_KEY_SIZE - 1)
        return false;
    memcpy(out, ns, ns_length);
    out[ns_length] = '/';
    memcpy(out + ns_length + 1, key, key_length);
    out[ns_length + 1 + key_length] = '\0';
    return true;
}

uint64_t record_mark_text_hash(const char *text)
{
    uint64_t hash = 0xcbf29ce484222325ull; // FNV-1a 64 offset basis
    for (const char *p = text != nullptr ? text : ""; *p != '\0'; ++p)
    {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 0x100000001b3ull; // FNV prime
    }
    return hash != 0 ? hash : 1;
}
