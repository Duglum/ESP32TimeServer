// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// The own web certificate in NVS: chain and private key as ONE blob, "<chain PEM>\0<key PEM>\0". NVS of ESP-IDF 6.1.1
// changes one blob all or nothing (the new version is written in full, the old one is erased only after it; at the
// start a new version without its index is removed and of two complete versions the newer one is kept), so after a
// failed store or a power loss the next start finds the old pair or the new pair, never a chain with another key.
// Before 2026-11 chain and key were two blobs, written one after the other (see web_tls.cpp).
// No ESP-IDF dependencies: also built by the host tests (testing/host/web_tls_pair_test.cpp).

#pragma once

#include <cstddef>
#include <cstdint>

// Bytes of the blob for a chain and a key of these lengths (characters, without their NUL)
constexpr size_t web_tls_pair_size(size_t cert_length, size_t key_length)
{
    return cert_length + 1 + key_length + 1;
}

// Writes "<cert>\0<key>\0" to out. Returns the bytes written, 0 when out is too small or a text is empty.
size_t web_tls_pair_pack(const char *cert, const char *key, uint8_t *out, size_t out_size);

// Splits a stored blob: cert and key point into data (each terminated by its NUL). False when the blob is not two
// non-empty texts that end with the last byte of the blob.
bool web_tls_pair_split(const uint8_t *data, size_t size, const char *&cert, const char *&key);

// NVS entries (32 bytes) a blob of size bytes takes at most when it is written: the data (each chunk rounded up to a
// whole entry), one header per chunk and the index. A chunk holds at most 4000 bytes (one page of 126 entries less its
// header); the first chunk fills the rest of the page in use, so there is at most one chunk more than size / 4000.
size_t web_tls_nvs_entries_max(size_t size);

// NVS entries a stored blob of size bytes takes at least: its data, one header and the index
size_t web_tls_nvs_entries_min(size_t size);

// Room check before a blob of new_size bytes is stored next to the old data (old_entries_min: what is erased once the
// new blob is stored). available: nvs_stats_t.available_entries. reserve: the entries NVS keeps free afterwards (for
// writing the settings again). A store that does not grow the data is only refused when the new blob does not fit
// next to the old data. needed: the entries checked against available.
bool web_tls_nvs_room(size_t available, size_t new_size, size_t old_entries_min, size_t reserve, size_t &needed);
