// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Memory that held a secret (key material, a password, a shared secret, a TOTP secret or recovery code, or a text
// with such values: a settings set, its JSON, a request body, a configuration file, an NVS page) is zeroed before it
// is freed or goes out of scope, always with mbedtls_platform_zeroize(). A plain memset() of memory that is not read
// again (a buffer right before free(), a local right before the end of its scope) is a dead store that the compiler
// removes at -O2: measured in this firmware on 2026-11-23 (the request body in read_json(), the DER of the TLS key,
// the TOTP HMAC and key, the PBKDF2 block, the NVS page of the purge, ...). mbedtls_platform_zeroize() is a function
// of the mbedTLS library, another translation unit (the build has no LTO), so the call stays; mbedTLS itself writes
// the zeros through a volatile pointer to memset() with a compiler barrier after it (platform_util.c).
//
// So that a later change cannot forget it, the zeroing is built in where memory is given back:
//  - big_delete() (sys/big_alloc.h) zeroes every object after its destructor;
//  - every block cJSON frees (the nodes and strings of a tree, a printed text, the buffers of a print) goes through
//    secret_free(): sys/cjson_mem.cpp sets it as the free function of cJSON;
//  - every block mbedTLS frees (the TLS record buffers among them) goes through secret_free(): sys/mbedtls_mem.cpp
//    registers it as the free function of mbedTLS (main.cpp calls mbedtls_mem_install());
//  - a stack buffer of a function with several exits gets a secret_wipe_t, which zeroes it at every exit of its scope.
// The secrets in use for the whole run (the settings in use, the keys of the TLS identity, the tables of sessions and
// TOTP tokens) stay where they are; ESP-IDF and esp-mqtt zero (or do not zero) their own memory themselves.
#pragma once

#include <cstddef>

extern "C"
{
#include "mbedtls/platform_util.h"
}

// Zeroes the whole heap block (heap_caps_get_allocated_size(): all the bytes the heap gave, not only those asked
// for) and frees it. nullptr: nothing. For memory of malloc(), calloc() and heap_caps_*() only.
void secret_free(void *block);

// Zeroes size bytes at data when the scope ends, whichever way it is left
class secret_wipe_t
{
public:
    secret_wipe_t(void *data, size_t size) : m_data(data), m_size(size) {}
    ~secret_wipe_t() { mbedtls_platform_zeroize(m_data, m_size); }
    secret_wipe_t(const secret_wipe_t &) = delete;
    secret_wipe_t &operator=(const secret_wipe_t &) = delete;

private:
    void *m_data;
    size_t m_size;
};
