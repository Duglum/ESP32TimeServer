// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// PTP AUTHENTICATION TLV (IEEE 1588-2019 16.14, immediate security processing) with HMAC-SHA256-128, compatible with
// linuxptp (sa_file key type SHA256-128). Idea and TLV layout from upstream 4.0 (custom/ptp_l2_auth.cpp); the fork
// takes the keys and the policy from the web settings at run time (seqlock, like ntp_auth.cpp), computes the HMAC
// with the PSA hash calls (no PSA key store, so keys can be replaced while the PTP task runs) and uses it for every
// transport (Ethernet, UDP/IPv4, UDP/IPv6).
//
// TLV (26 bytes, last in the message): tlvType 0x8009, lengthField 22, securityParameterPointer (spp),
// secParamIndicator 0, keyID (32 bit), ICV (16 bytes). The ICV is the HMAC-SHA256 of the message from its first
// byte to the ICV (messageLength already includes the TLV), truncated to 16 bytes; with mutable_correction the
// correctionField counts as zero (linuxptp allow_mutable 1). Signed messages have minorVersionPTP 1.

#pragma once

#include <cstddef>
#include <cstdint>

static constexpr size_t PTP_AUTH_TLV_SIZE = 26;
static constexpr size_t PTP_AUTH_ICV_SIZE = 16;
static constexpr size_t PTP_AUTH_MAX_KEYS = 4;
static constexpr size_t PTP_AUTH_KEY_LENGTH = 32; // letters or digits, used as they are as the HMAC key

enum class ptp_auth_result_t : uint8_t
{
    unauthenticated,         // no TLV, authentication not required
    valid,                   // TLV with a known key and a correct ICV
    authentication_required, // no TLV while authentication is required
    malformed,               // wrong length, TLV, spp, secParamIndicator, version
    unavailable,             // TLV, but authentication is off or has no usable key
    unknown_key,
    invalid_mac,
};

struct ptp_auth_key_input_t
{
    uint32_t key_id; // 1 to 65535 (the web settings)
    const char *key; // PTP_AUTH_KEY_LENGTH letters or digits, NUL terminated
};

struct ptp_auth_config_t
{
    bool enabled = false;            // sign outgoing messages, check signed incoming ones
    bool required = false;           // drop unsigned Delay_Req / Announce (only with enabled)
    bool mutable_correction = false; // correctionField not covered by the ICV (linuxptp allow_mutable 1)
    uint8_t spp = 0;                 // securityParameterPointer of the security association
    uint32_t active_key_id = 0;      // key that signs the outgoing messages
};

// Takes over policy and keys (any task; lock-free for the PTP task). Invalid keys are skipped. Returns true if
// outgoing messages are signed afterwards (enabled, PSA ready, the active key among the valid keys). required stays
// in force without a usable key (fail closed).
bool ptp_auth_configure(const ptp_auth_config_t &config, const ptp_auth_key_input_t *keys, size_t count);
bool ptp_auth_available();
bool ptp_auth_required();

// A received message (message points at the PTP header, length = its messageLength). Only types with a known body
// size are accepted: Sync / Delay_Req / Follow_Up 44, Delay_Resp 54, Announce 64 bytes.
ptp_auth_result_t ptp_auth_verify(const uint8_t *message, size_t length);

// Appends the TLV to a complete unsigned message of *length bytes (minorVersionPTP must already be 1), sets its
// messageLength and the ICV. false: not available, buffer too small or not a known message.
bool ptp_auth_append(uint8_t *message, size_t capacity, size_t *length);
