// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// See ptp_auth.h.

#include "ptp_auth.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "esp_log.h"
#include "psa/crypto.h"

namespace
{
    static const char *TAG = "ptp_auth";
    static constexpr uint16_t TLV_AUTHENTICATION = 0x8009;
    static constexpr size_t HMAC_BLOCK_SIZE = 64;
    static constexpr size_t SHA256_SIZE = 32;

    struct auth_key_t
    {
        uint32_t id;
        uint8_t value[PTP_AUTH_KEY_LENGTH];
    };

    // Everything the PTP task reads: written by ptp_auth_configure(), read under the seqlock s_sequence
    struct state_t
    {
        size_t count;
        auth_key_t keys[PTP_AUTH_MAX_KEYS];
        uint32_t active_key_id;
        uint8_t spp;
        bool mutable_correction;
    };

    static state_t s_state{};
    static std::atomic<uint32_t> s_sequence{0};
    static std::atomic<bool> s_available{false};
    static std::atomic<bool> s_required{false};
    static std::mutex s_writer_mutex;

    // What one message needs: the key (the active one, or the one of a received TLV) and the parameters
    struct snapshot_t
    {
        bool found;
        auth_key_t key;
        uint8_t spp;
        bool mutable_correction;
    };

    // key_id 0: the active key
    static void load(uint32_t key_id, snapshot_t &out)
    {
        uint32_t s1, s2;
        do
        {
            s1 = s_sequence.load(std::memory_order_acquire);
            out.found = false;
            const uint32_t wanted = key_id != 0 ? key_id : s_state.active_key_id;
            const size_t count = s_state.count <= PTP_AUTH_MAX_KEYS ? s_state.count : 0;
            for (size_t i = 0; i < count; ++i)
            {
                if (s_state.keys[i].id == wanted)
                {
                    out.key = s_state.keys[i];
                    out.found = true;
                    break;
                }
            }
            out.spp = s_state.spp;
            out.mutable_correction = s_state.mutable_correction;
            std::atomic_thread_fence(std::memory_order_acquire);
            s2 = s_sequence.load(std::memory_order_relaxed);
        } while (s1 != s2 || (s1 & 1u));
    }

    static uint16_t read_u16(const uint8_t *data)
    {
        return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
    }

    // Body sizes of the message types (IEEE 1588-2019 13.6 to 13.10), 0 = not handled
    static size_t body_size(uint8_t type)
    {
        switch (type & 0x0f)
        {
        case 0: // Sync
        case 1: // Delay_Req
        case 8: // Follow_Up
            return 44;
        case 9: // Delay_Resp
            return 54;
        case 11: // Announce
            return 64;
        default:
            return 0;
        }
    }

    static bool key_value_is_valid(const char *value)
    {
        size_t length = 0;
        while (length <= PTP_AUTH_KEY_LENGTH && value[length] != '\0')
        {
            const char c = value[length++];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
                return false;
        }
        return length == PTP_AUTH_KEY_LENGTH;
    }

    static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
    {
        uint8_t difference = 0;
        for (size_t i = 0; i < length; ++i)
            difference |= left[i] ^ right[i];
        return difference == 0;
    }

    // One SHA-256 over pad (one block) and the data; with zero_correction the bytes 8 to 15 of data (correctionField
    // of the PTP header) count as zero. Each step is checked on its own.
    static bool hash_block_and_data(const uint8_t pad[HMAC_BLOCK_SIZE], const uint8_t *data, size_t length, bool zero_correction,
                                    uint8_t digest[SHA256_SIZE])
    {
        static constexpr uint8_t zeros[8] = {};
        size_t digest_length = 0;
        psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
        psa_status_t status = psa_hash_setup(&operation, PSA_ALG_SHA_256);
        if (status == PSA_SUCCESS)
            status = psa_hash_update(&operation, pad, HMAC_BLOCK_SIZE);
        if (zero_correction)
        {
            if (status == PSA_SUCCESS)
                status = psa_hash_update(&operation, data, 8);
            if (status == PSA_SUCCESS)
                status = psa_hash_update(&operation, zeros, sizeof(zeros));
            if (status == PSA_SUCCESS)
                status = psa_hash_update(&operation, data + 16, length - 16);
        }
        else if (status == PSA_SUCCESS)
            status = psa_hash_update(&operation, data, length);
        if (status == PSA_SUCCESS)
            status = psa_hash_finish(&operation, digest, SHA256_SIZE, &digest_length);
        psa_hash_abort(&operation);
        return status == PSA_SUCCESS && digest_length == SHA256_SIZE;
    }

    // HMAC-SHA256 (RFC 2104; the key is shorter than a block) of message[0, length), truncated to the ICV size.
    // length is at least 34 (a PTP header).
    static bool calculate_icv(const auth_key_t &key, const uint8_t *message, size_t length, bool zero_correction,
                              uint8_t icv[PTP_AUTH_ICV_SIZE])
    {
        uint8_t pad[HMAC_BLOCK_SIZE];
        uint8_t inner[SHA256_SIZE];
        memset(pad, 0x36, sizeof(pad));
        for (size_t i = 0; i < PTP_AUTH_KEY_LENGTH; ++i)
            pad[i] ^= key.value[i];
        bool ok = hash_block_and_data(pad, message, length, zero_correction, inner);
        if (ok)
        {
            for (size_t i = 0; i < HMAC_BLOCK_SIZE; ++i)
                pad[i] ^= 0x36 ^ 0x5c;
            uint8_t outer[SHA256_SIZE];
            ok = hash_block_and_data(pad, inner, sizeof(inner), false, outer);
            if (ok)
                memcpy(icv, outer, PTP_AUTH_ICV_SIZE);
            memset(outer, 0, sizeof(outer));
        }
        memset(pad, 0, sizeof(pad));
        memset(inner, 0, sizeof(inner));
        return ok;
    }
} // namespace

bool ptp_auth_configure(const ptp_auth_config_t &config, const ptp_auth_key_input_t *keys, size_t count)
{
    state_t next{};
    bool active_found = false;
    for (size_t i = 0; keys != nullptr && i < count && next.count < PTP_AUTH_MAX_KEYS; ++i)
    {
        bool duplicate = false;
        for (size_t k = 0; k < next.count; ++k)
            duplicate = duplicate || next.keys[k].id == keys[i].key_id;
        if (keys[i].key_id < 1 || keys[i].key_id > 65535 || keys[i].key == nullptr || duplicate || !key_value_is_valid(keys[i].key))
        {
            ESP_LOGE(TAG, "PTP key %u of the web settings is not valid and is not used", static_cast<unsigned int>(i + 1));
            continue;
        }
        auth_key_t &key = next.keys[next.count++];
        key.id = keys[i].key_id;
        memcpy(key.value, keys[i].key, PTP_AUTH_KEY_LENGTH);
        active_found = active_found || key.id == config.active_key_id;
    }
    next.active_key_id = config.active_key_id;
    next.spp = config.spp;
    next.mutable_correction = config.mutable_correction;

    // psa_crypto_init() may be called again (ntp_auth_initialize() calls it at the start as well)
    const bool available = config.enabled && active_found && psa_crypto_init() == PSA_SUCCESS;
    {
        std::lock_guard<std::mutex> lock(s_writer_mutex);
        // The policy first: switching to required never answers an unsigned request in between
        s_required.store(config.enabled && config.required, std::memory_order_release);
        s_available.store(false, std::memory_order_release);
        const uint32_t s = s_sequence.load(std::memory_order_relaxed);
        s_sequence.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        s_state = next;
        s_sequence.store(s + 2, std::memory_order_release);
        s_available.store(available, std::memory_order_release);
    }
    memset(&next, 0, sizeof(next));
    if (config.enabled && !available)
        ESP_LOGE(TAG, "PTP authentication is on but the active key %lu is not usable: PTP messages are not signed%s",
                 static_cast<unsigned long>(config.active_key_id), config.required ? " and not answered" : "");
    return available;
}

bool ptp_auth_available()
{
    return s_available.load(std::memory_order_acquire);
}

bool ptp_auth_required()
{
    return s_required.load(std::memory_order_acquire);
}

ptp_auth_result_t ptp_auth_verify(const uint8_t *message, size_t length)
{
    if (message == nullptr || length < 34 || read_u16(message + 2) != length)
        return ptp_auth_result_t::malformed;
    const size_t body = body_size(message[0]);
    if (body == 0 || length < body)
        return ptp_auth_result_t::malformed;
    if (length == body)
        return s_required.load(std::memory_order_relaxed) ? ptp_auth_result_t::authentication_required
                                                          : ptp_auth_result_t::unauthenticated;
    // Exactly one TLV, the AUTHENTICATION TLV, without the optional fields (secParamIndicator 0); minorVersionPTP 1
    if (length != body + PTP_AUTH_TLV_SIZE || message[1] != 0x12)
        return ptp_auth_result_t::malformed;
    const uint8_t *tlv = message + body;
    if (read_u16(tlv) != TLV_AUTHENTICATION || read_u16(tlv + 2) != PTP_AUTH_TLV_SIZE - 4 || tlv[5] != 0)
        return ptp_auth_result_t::malformed;
    if (!s_available.load(std::memory_order_acquire))
        return ptp_auth_result_t::unavailable;
    const uint32_t key_id = (static_cast<uint32_t>(tlv[6]) << 24) | (static_cast<uint32_t>(tlv[7]) << 16) |
                            (static_cast<uint32_t>(tlv[8]) << 8) | tlv[9];
    if (key_id == 0)
        return ptp_auth_result_t::unknown_key; // key ID 0 is not allowed (linuxptp: 1 to 2^32-1)
    snapshot_t snapshot{};
    load(key_id, snapshot);
    if (tlv[4] != snapshot.spp || !snapshot.found)
    {
        const bool other_association = tlv[4] != snapshot.spp;
        memset(&snapshot, 0, sizeof(snapshot));
        return other_association ? ptp_auth_result_t::malformed : ptp_auth_result_t::unknown_key;
    }
    uint8_t icv[PTP_AUTH_ICV_SIZE];
    const bool valid = calculate_icv(snapshot.key, message, length - PTP_AUTH_ICV_SIZE, snapshot.mutable_correction, icv) &&
                       constant_time_equal(icv, tlv + 10, PTP_AUTH_ICV_SIZE);
    memset(&snapshot, 0, sizeof(snapshot));
    return valid ? ptp_auth_result_t::valid : ptp_auth_result_t::invalid_mac;
}

bool ptp_auth_append(uint8_t *message, size_t capacity, size_t *length)
{
    if (message == nullptr || length == nullptr || *length < 34 || *length > capacity ||
        capacity - *length < PTP_AUTH_TLV_SIZE || body_size(message[0]) != *length ||
        read_u16(message + 2) != *length || message[1] != 0x12 || !s_available.load(std::memory_order_acquire))
        return false;
    snapshot_t snapshot{};
    load(0, snapshot);
    if (!snapshot.found)
        return false;
    const size_t total = *length + PTP_AUTH_TLV_SIZE;
    uint8_t *tlv = message + *length;
    tlv[0] = static_cast<uint8_t>(TLV_AUTHENTICATION >> 8);
    tlv[1] = static_cast<uint8_t>(TLV_AUTHENTICATION);
    tlv[2] = 0;
    tlv[3] = PTP_AUTH_TLV_SIZE - 4;
    tlv[4] = snapshot.spp;
    tlv[5] = 0; // immediate security processing, no optional fields
    for (size_t i = 0; i < 4; ++i)
        tlv[6 + i] = static_cast<uint8_t>(snapshot.key.id >> (24 - 8 * i));
    message[2] = static_cast<uint8_t>(total >> 8);
    message[3] = static_cast<uint8_t>(total);
    const bool ok = calculate_icv(snapshot.key, message, total - PTP_AUTH_ICV_SIZE, snapshot.mutable_correction, tlv + 10);
    memset(&snapshot, 0, sizeof(snapshot));
    if (!ok)
    {
        // Back to the unsigned message (the caller does not send it: no unsigned fallback)
        message[2] = static_cast<uint8_t>(*length >> 8);
        message[3] = static_cast<uint8_t>(*length);
        memset(tlv, 0, PTP_AUTH_TLV_SIZE);
        return false;
    }
    *length = total;
    return true;
}
