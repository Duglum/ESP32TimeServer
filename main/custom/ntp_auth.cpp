// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "ntp_auth.h"
#include "ESP32TimeServerSettings.h"

#include <atomic>
#include <cinttypes>
#include <cstring>
#include <iterator>
#include <mutex>

#include "esp_log.h"
#include "psa/crypto.h"

namespace
{
    static constexpr uint8_t NTP_AUTH_ALGORITHM_SHA256_160 = 1;
    static constexpr uint8_t NTP_AUTH_KEY_ACTIVE = 1;
    static constexpr size_t SYMMETRIC_KEY_LENGTH = 32;
    static const char *TAG = "ntp_auth";

    static constexpr unsigned int number_of_symmetric_keys = std::size(symmetric_keys);

    struct key_set_t
    {
        size_t count;
        ntp_auth_key_t keys[NTP_AUTH_MAX_KEYS];
    };

    // Keys of ESP32TimeServerSettingsSecrets.h (validated once)
    static key_set_t s_compiled{};
    // Key set in use. Written by ntp_auth_configure() (web admin or start), read for every authenticated NTP
    // request: seqlock, so the NTP path never blocks and never sees a half written key.
    static key_set_t s_active{};
    static std::atomic<uint32_t> s_sequence{0};
    static std::atomic<bool> s_available{false};
    static bool s_initialized = false;
    static bool s_crypto_ready = false;
    static std::mutex s_writer_mutex;

    static uint32_t read_u32_be(const uint8_t *value)
    {
        return (static_cast<uint32_t>(value[0]) << 24) | (static_cast<uint32_t>(value[1]) << 16) |
               (static_cast<uint32_t>(value[2]) << 8) | static_cast<uint32_t>(value[3]);
    }

    static void write_u32_be(uint8_t *value, uint32_t number)
    {
        value[0] = static_cast<uint8_t>(number >> 24);
        value[1] = static_cast<uint8_t>(number >> 16);
        value[2] = static_cast<uint8_t>(number >> 8);
        value[3] = static_cast<uint8_t>(number);
    }

    static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
    {
        uint8_t difference = 0;
        for (size_t index = 0; index < length; ++index)
            difference |= left[index] ^ right[index];
        return difference == 0;
    }

    // Copies the key with this ID out of the active set (consistent copy, see s_sequence)
    static bool find_key(uint32_t key_id, ntp_auth_key_t &key)
    {
        bool found = false;
        uint32_t s1, s2;
        do
        {
            found = false;
            s1 = s_sequence.load(std::memory_order_acquire);
            const size_t count = s_active.count <= NTP_AUTH_MAX_KEYS ? s_active.count : 0;
            for (size_t index = 0; index < count; ++index)
            {
                if (s_active.keys[index].key_id == key_id && s_active.keys[index].lifecycle == NTP_AUTH_KEY_ACTIVE)
                {
                    key = s_active.keys[index];
                    found = true;
                    break;
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = s_sequence.load(std::memory_order_relaxed);
        } while (s1 != s2 || (s1 & 1u));
        return found;
    }

    // Consistent copy of the whole active set (status and debug output only)
    static void snapshot(key_set_t &out)
    {
        uint32_t s1, s2;
        do
        {
            s1 = s_sequence.load(std::memory_order_acquire);
            out = s_active;
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = s_sequence.load(std::memory_order_relaxed);
        } while (s1 != s2 || (s1 & 1u));
        if (out.count > NTP_AUTH_MAX_KEYS)
            out.count = 0;
    }

    static bool key_value_is_valid(const char *value, size_t capacity)
    {
        size_t length = 0;
        while (length < capacity && value[length] != '\0')
        {
            const char character = value[length++];
            if (!((character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')))
                return false;
        }
        return length == SYMMETRIC_KEY_LENGTH && (length == capacity || value[length] == '\0');
    }

    static bool calculate_digest(const ntp_auth_key_t &key, const uint8_t *input, size_t input_length, uint8_t digest[NTP_AUTH_DIGEST_SIZE])
    {
        uint8_t full_digest[32] = {};
        size_t full_digest_length = 0;
        psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
        // Each step is checked on its own. (The former single expression converted the result of the && chain to
        // a psa_status_t, so a failed setup or update gave 0 = PSA_SUCCESS; only the length check caught it.)
        psa_status_t status = psa_hash_setup(&operation, PSA_ALG_SHA_256);
        if (status == PSA_SUCCESS)
            status = psa_hash_update(&operation, key.key, key.key_length);
        if (status == PSA_SUCCESS)
            status = psa_hash_update(&operation, input, input_length);
        if (status == PSA_SUCCESS)
            status = psa_hash_finish(&operation, full_digest, sizeof(full_digest), &full_digest_length);
        psa_hash_abort(&operation);
        if (status != PSA_SUCCESS || full_digest_length != sizeof(full_digest))
            return false;
        memcpy(digest, full_digest, NTP_AUTH_DIGEST_SIZE);
        return true;
    }
} // namespace

bool ntp_auth_initialize()
{
    if (s_initialized)
        return s_crypto_ready;

    s_initialized = true;

    if (number_of_symmetric_keys < 1 || number_of_symmetric_keys > NTP_AUTH_MAX_KEYS)
    {
        ESP_LOGE(TAG, "Invalid number of symmetric keys: %u (1 to %u are supported)", number_of_symmetric_keys,
                 static_cast<unsigned int>(NTP_AUTH_MAX_KEYS));
    }
    else
    {
        for (size_t index = 0; index < number_of_symmetric_keys; ++index)
        {
            const symmetric_key_t &source = symmetric_keys[index];
            bool valid = true;
            if (source.key_id < 1 || source.key_id > 65535)
            {
                ESP_LOGE(TAG, "Symmetric key %zu: key_id must be between 1 and 65535 (inclusive)(got %" PRIu32 ")", index + 1, source.key_id);
                valid = false;
            }
            if (!key_value_is_valid(source.key_value, sizeof(source.key_value)))
            {
                ESP_LOGE(TAG, "Symmetric key %zu: key_value must contain exactly %zu letters or digits", index + 1, SYMMETRIC_KEY_LENGTH);
                valid = false;
            }
            for (size_t other = 0; other < s_compiled.count; ++other)
            {
                if (s_compiled.keys[other].key_id == source.key_id)
                {
                    ESP_LOGE(TAG, "Symmetric key %zu: duplicate key_id %" PRIu32, index + 1, source.key_id);
                    valid = false;
                    break;
                }
            }
            if (!valid)
                continue;

            ntp_auth_key_t &key = s_compiled.keys[s_compiled.count++];
            key = {source.key_id, NTP_AUTH_ALGORITHM_SHA256_160, static_cast<uint8_t>(SYMMETRIC_KEY_LENGTH), NTP_AUTH_KEY_ACTIVE, {}};
            memcpy(key.key, source.key_value, SYMMETRIC_KEY_LENGTH);
        }
    }
    s_crypto_ready = psa_crypto_init() == PSA_SUCCESS;
    return s_crypto_ready;
}

bool ntp_auth_configure(bool enabled, bool use_override_keys, const ntp_auth_key_input_t *keys, size_t count)
{
    key_set_t next{};
    if (use_override_keys)
    {
        for (size_t i = 0; keys != nullptr && i < count && next.count < NTP_AUTH_MAX_KEYS; ++i)
        {
            bool duplicate = false;
            for (size_t k = 0; k < next.count; ++k)
                duplicate = duplicate || next.keys[k].key_id == keys[i].key_id;
            if (keys[i].key_id < 1 || keys[i].key_id > 65535 || keys[i].key == nullptr || duplicate ||
                !key_value_is_valid(keys[i].key, SYMMETRIC_KEY_LENGTH + 1))
            {
                ESP_LOGE(TAG, "NTP key %u of the web settings is not valid and is not used", static_cast<unsigned int>(i + 1));
                continue;
            }
            ntp_auth_key_t &key = next.keys[next.count++];
            key = {keys[i].key_id, NTP_AUTH_ALGORITHM_SHA256_160, static_cast<uint8_t>(SYMMETRIC_KEY_LENGTH), NTP_AUTH_KEY_ACTIVE, {}};
            memcpy(key.key, keys[i].key, SYMMETRIC_KEY_LENGTH);
        }
    }
    else
        next = s_compiled;

    const bool available = enabled && s_crypto_ready && next.count != 0;
    {
        std::lock_guard<std::mutex> lock(s_writer_mutex);
        // Switch off first, so no request is answered with a key that is half replaced
        s_available.store(false, std::memory_order_release);
        const uint32_t s = s_sequence.load(std::memory_order_relaxed);
        s_sequence.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        s_active = next;
        s_sequence.store(s + 2, std::memory_order_release);
        s_available.store(available, std::memory_order_release);
    }
    memset(&next, 0, sizeof(next));
    return available;
}

bool ntp_auth_available()
{
    return s_available.load(std::memory_order_acquire);
}

size_t ntp_auth_compiled_key_count()
{
    return s_compiled.count;
}

size_t ntp_auth_key_count()
{
    if (!ntp_auth_available())
        return 0;
    key_set_t keys{};
    snapshot(keys);
    const size_t count = keys.count;
    memset(&keys, 0, sizeof(keys));
    return count;
}

uint32_t ntp_auth_default_key_id()
{
    if (!ntp_auth_available())
        return 0;
    key_set_t keys{};
    snapshot(keys);
    const uint32_t key_id = keys.count != 0 ? keys.keys[0].key_id : 0;
    memset(&keys, 0, sizeof(keys));
    return key_id;
}

bool ntp_auth_key_hex(size_t index, uint32_t *key_id, char *output, size_t output_size)
{
    if (!ntp_auth_available() || key_id == nullptr || output == nullptr)
        return false;
    key_set_t keys{};
    snapshot(keys);
    bool ok = index < keys.count && output_size >= static_cast<size_t>(keys.keys[index].key_length) * 2 + 1;
    if (ok)
    {
        const ntp_auth_key_t &key = keys.keys[index];
        static constexpr char hex_digits[] = "0123456789ABCDEF";
        for (size_t character_index = 0; character_index < key.key_length; ++character_index)
        {
            output[character_index * 2] = hex_digits[key.key[character_index] >> 4];
            output[character_index * 2 + 1] = hex_digits[key.key[character_index] & 0x0F];
        }
        output[key.key_length * 2] = '\0';
        *key_id = key.key_id;
    }
    memset(&keys, 0, sizeof(keys));
    return ok;
}

bool ntp_auth_default_key_hex(char *output, size_t output_size)
{
    uint32_t key_id = 0;
    return ntp_auth_key_hex(0, &key_id, output, output_size);
}

bool ntp_auth_default_key_ascii(char *output, size_t output_size)
{
    if (!ntp_auth_available() || output == nullptr)
        return false;
    key_set_t keys{};
    snapshot(keys);
    bool ok = keys.count != 0 && output_size >= static_cast<size_t>(keys.keys[0].key_length) + 1;
    for (size_t index = 0; ok && index < keys.keys[0].key_length; ++index)
    {
        const uint8_t character = keys.keys[0].key[index];
        if (character < 0x21 || character > 0x7E)
            ok = false;
        else
            output[index] = static_cast<char>(character);
    }
    if (ok)
        output[keys.keys[0].key_length] = '\0';
    memset(&keys, 0, sizeof(keys));
    return ok;
}

ntp_auth_result_t ntp_auth_verify_request(const uint8_t *packet, size_t packet_length, uint8_t version, uint32_t *key_id)
{
    if (key_id != nullptr)
        *key_id = 0;
    if (packet == nullptr || packet_length < 48)
        return ntp_auth_result_t::malformed;
    if (packet_length == 48)
        return ntp_auth_result_t::unauthenticated;
    if (version != 4 || packet_length != 48 + NTP_AUTH_TRAILER_SIZE)
        return ntp_auth_result_t::malformed;
    if (!ntp_auth_available())
        return ntp_auth_result_t::unavailable;

    const size_t key_id_offset = packet_length - NTP_AUTH_TRAILER_SIZE;
    const uint32_t requested_key_id = read_u32_be(packet + key_id_offset);
    ntp_auth_key_t key{};
    if (!find_key(requested_key_id, key))
        return ntp_auth_result_t::unknown_key;

    uint8_t digest[NTP_AUTH_DIGEST_SIZE] = {};
    const bool valid = calculate_digest(key, packet, key_id_offset, digest) &&
                       constant_time_equal(digest, packet + key_id_offset + NTP_AUTH_KEY_ID_SIZE, NTP_AUTH_DIGEST_SIZE);
    memset(&key, 0, sizeof(key));
    if (!valid)
        return ntp_auth_result_t::invalid_mac;

    if (key_id != nullptr)
        *key_id = requested_key_id;
    return ntp_auth_result_t::valid;
}

bool ntp_auth_append_response(uint8_t *packet, size_t packet_capacity, size_t *packet_length, uint32_t key_id)
{
    if (packet == nullptr || packet_length == nullptr || *packet_length != 48 || packet_capacity < 48 + NTP_AUTH_TRAILER_SIZE || !ntp_auth_available())
        return false;

    ntp_auth_key_t key{};
    if (!find_key(key_id, key))
        return false;

    write_u32_be(packet + 48, key_id);
    const bool ok = calculate_digest(key, packet, 48, packet + 48 + NTP_AUTH_KEY_ID_SIZE);
    memset(&key, 0, sizeof(key));
    if (!ok)
        return false;
    *packet_length = 48 + NTP_AUTH_TRAILER_SIZE;
    return true;
}
