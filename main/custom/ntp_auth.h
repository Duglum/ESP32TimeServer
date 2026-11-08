// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include <cstddef>
#include <cstdint>

static constexpr size_t NTP_AUTH_KEY_ID_SIZE = 4;
static constexpr size_t NTP_AUTH_DIGEST_SIZE = 20;
static constexpr size_t NTP_AUTH_TRAILER_SIZE = NTP_AUTH_KEY_ID_SIZE + NTP_AUTH_DIGEST_SIZE;
static constexpr size_t NTP_AUTH_MAX_KEYS = 8;
static constexpr size_t NTP_AUTH_MAX_KEY_SIZE = 64;

enum class ntp_auth_result_t : uint8_t
{
    unauthenticated,
    valid,
    malformed,
    unavailable,
    unknown_key,
    invalid_mac,
    authentication_required, // unsigned request (NTPv3 or NTPv4 without MAC) while authentication is required
};

struct ntp_auth_key_t
{
    uint32_t key_id;
    uint8_t algorithm;
    uint8_t key_length;
    uint8_t lifecycle;
    uint8_t key[NTP_AUTH_MAX_KEY_SIZE];
};

// Initializes PSA. Authentication stays off until ntp_auth_configure() switches it on.
bool ntp_auth_initialize();

// One key of the web settings (key: 32 letters or digits, NUL terminated)
struct ntp_auth_key_input_t
{
    uint32_t key_id;
    const char *key;
};

// Run-time switch, policy and the keys of the web settings (count of them in keys). Lock-free for the NTP path.
// required (only with enabled): unsigned requests give ntp_auth_result_t::authentication_required and are not
// answered. It stays in force when no key is usable (fail closed: then no request is answered at all). Returns true
// if authentication is available afterwards.
bool ntp_auth_configure(bool enabled, bool required, const ntp_auth_key_input_t *keys, size_t count);
bool ntp_auth_available();
bool ntp_auth_required();
size_t ntp_auth_key_count();
bool ntp_auth_key_hex(size_t index, uint32_t *key_id, char *output, size_t output_size);
uint32_t ntp_auth_default_key_id();
bool ntp_auth_default_key_hex(char *output, size_t output_size);
bool ntp_auth_default_key_ascii(char *output, size_t output_size);
ntp_auth_result_t ntp_auth_verify_request(const uint8_t *packet, size_t packet_length, uint8_t version, uint32_t *key_id);
bool ntp_auth_append_response(uint8_t *packet, size_t packet_capacity, size_t *packet_length, uint32_t key_id);
