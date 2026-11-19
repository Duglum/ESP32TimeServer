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

bool web_tls_nvs_room(size_t available, size_t new_size, size_t old_entries_min, size_t reserve, size_t &needed)
{
    const size_t entries = web_tls_nvs_entries_max(new_size);
    needed = entries;
    // Afterwards the old data is erased: a larger blob also needs the reserve
    if (entries > old_entries_min && entries - old_entries_min + reserve > needed)
        needed = entries - old_entries_min + reserve;
    return available >= needed;
}
