// ESP32 Time Server - host tests: the PSA hash calls of main/custom/ntp_auth.cpp, with a plain SHA-256 (FIPS 180-4).
// Only what ntp_auth.cpp uses; checked by ntp_auth_test against known answers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_BAD_STATE ((psa_status_t)-137)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t)-138)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t)-134)
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)

struct psa_hash_operation_t
{
    bool active;
    uint32_t state[8];
    uint8_t block[64];
    size_t block_used;
    uint64_t total_bytes;
};
#define PSA_HASH_OPERATION_INIT psa_hash_operation_t{}

// Set by a test to make the next calls fail (0 = no failure)
inline psa_status_t &host_psa_forced_status()
{
    static psa_status_t status = PSA_SUCCESS;
    return status;
}

inline psa_status_t psa_crypto_init() { return host_psa_forced_status(); }

namespace host_sha256
{
    inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    inline void compress(uint32_t state[8], const uint8_t block[64])
    {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) | block[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
} // namespace host_sha256

inline psa_status_t psa_hash_setup(psa_hash_operation_t *operation, psa_algorithm_t algorithm)
{
    if (host_psa_forced_status() != PSA_SUCCESS)
        return host_psa_forced_status();
    if (operation->active)
        return PSA_ERROR_BAD_STATE;
    if (algorithm != PSA_ALG_SHA_256)
        return PSA_ERROR_NOT_SUPPORTED;
    static constexpr uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    *operation = psa_hash_operation_t{};
    memcpy(operation->state, initial, sizeof(initial));
    operation->active = true;
    return PSA_SUCCESS;
}

inline psa_status_t psa_hash_update(psa_hash_operation_t *operation, const uint8_t *input, size_t input_length)
{
    if (!operation->active)
        return PSA_ERROR_BAD_STATE;
    for (size_t i = 0; i < input_length; ++i)
    {
        operation->block[operation->block_used++] = input[i];
        if (operation->block_used == 64)
        {
            host_sha256::compress(operation->state, operation->block);
            operation->block_used = 0;
        }
    }
    operation->total_bytes += input_length;
    return PSA_SUCCESS;
}

inline psa_status_t psa_hash_finish(psa_hash_operation_t *operation, uint8_t *hash, size_t hash_size, size_t *hash_length)
{
    if (!operation->active)
        return PSA_ERROR_BAD_STATE;
    if (hash_size < 32)
        return PSA_ERROR_BUFFER_TOO_SMALL;
    const uint64_t bits = operation->total_bytes * 8;
    const uint8_t pad = 0x80;
    const uint8_t zero = 0;
    const uint64_t total = operation->total_bytes;
    psa_hash_update(operation, &pad, 1);
    while (operation->block_used != 56)
        psa_hash_update(operation, &zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i)
        length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    psa_hash_update(operation, length, 8);
    operation->total_bytes = total;
    for (int i = 0; i < 8; ++i)
    {
        hash[i * 4] = static_cast<uint8_t>(operation->state[i] >> 24);
        hash[i * 4 + 1] = static_cast<uint8_t>(operation->state[i] >> 16);
        hash[i * 4 + 2] = static_cast<uint8_t>(operation->state[i] >> 8);
        hash[i * 4 + 3] = static_cast<uint8_t>(operation->state[i]);
    }
    *hash_length = 32;
    operation->active = false;
    return PSA_SUCCESS;
}

inline psa_status_t psa_hash_abort(psa_hash_operation_t *operation)
{
    *operation = psa_hash_operation_t{};
    return PSA_SUCCESS;
}
