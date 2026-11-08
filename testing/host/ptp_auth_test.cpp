// ESP32 Time Server - host test of the PTP AUTHENTICATION TLV (main/custom/ptp_auth.cpp, see README.md): the TLV
// layout and the HMAC-SHA256-128 ICV against values of Python hmac (with and without the correctionField, linuxptp
// allow_mutable), signing of every message type the master sends, the checks of received messages (spp, key ID,
// lengths, version, changed bytes) and the mixed / required policy.
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "ptp_auth.h"
#include "ptp_message.h"
#include "psa/crypto.h"

static int s_checks = 0;
#define CHECK(condition)   \
    do                     \
    {                      \
        assert(condition); \
        ++s_checks;        \
    } while (0)

static const char KEY_3[] = "ZYXWVUTSRQPONMLKJIHGFEDCBA987654";
static const char KEY_9[] = "0123456789abcdefABCDEF0123456789";

static void hex(const char *text, uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        unsigned value = 0;
        sscanf(text + i * 2, "%2x", &value);
        out[i] = static_cast<uint8_t>(value);
    }
}

static bool configure(bool enabled, bool required, bool mutable_correction, uint32_t active = 3, uint8_t spp = 5)
{
    const ptp_auth_key_input_t keys[] = {{3, KEY_3}, {9, KEY_9}};
    ptp_auth_config_t config;
    config.enabled = enabled;
    config.required = required;
    config.mutable_correction = mutable_correction;
    config.spp = spp;
    config.active_key_id = active;
    return ptp_auth_configure(config, keys, 2);
}

// Delay_Req (44 bytes) with a correctionField, minorVersionPTP 1
static size_t delay_req(uint8_t *m)
{
    memset(m, 0, 44 + PTP_AUTH_TLV_SIZE);
    m[0] = 1;
    m[1] = 0x12;
    m[3] = 44;
    m[13] = 0x01;
    m[14] = 0x23;
    m[15] = 0x45;
    const uint8_t identity[8] = {0x00, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
    memcpy(m + 20, identity, 8);
    m[29] = 1;
    m[31] = 7;
    m[32] = 1;
    m[33] = 0x7f;
    return 44;
}

// The ICV of Python: hmac.new(key, message[:54], sha256).digest()[:16]
static void test_known_icv()
{
    uint8_t m[44 + PTP_AUTH_TLV_SIZE];
    for (const bool mutable_correction : {false, true})
    {
        CHECK(configure(true, false, mutable_correction));
        size_t length = delay_req(m);
        CHECK(ptp_auth_append(m, sizeof(m), &length) && length == 70);
        uint8_t expected[PTP_AUTH_TLV_SIZE] = {0x80, 0x09, 0x00, 0x16, 5, 0, 0, 0, 0, 3};
        hex(mutable_correction ? "ec0a0686e5bc5a8a1ab08624fdffac88" : "6130400726d8b12be0bfaec6bc200351", expected + 10, 16);
        CHECK(memcmp(m + 44, expected, sizeof(expected)) == 0);
        CHECK(m[2] == 0 && m[3] == 70);
        CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::valid);
        // The correctionField: covered without mutable_correction only
        m[15] ^= 0x01;
        CHECK(ptp_auth_verify(m, length) == (mutable_correction ? ptp_auth_result_t::valid : ptp_auth_result_t::invalid_mac));
        m[15] ^= 0x01;
    }
}

// Every message the master sends can be signed and verified; the builder sets minorVersionPTP 1
static void test_master_messages()
{
    CHECK(configure(true, false, false, 9));
    ptp::PortConfig port{};
    port.authenticated = true;
    port.identity[0] = 0xAA;
    uint8_t m[ptp::ANNOUNCE_SIZE + PTP_AUTH_TLV_SIZE];
    const uint8_t requester[10] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 1};
    const size_t sizes[] = {
        ptp::build_sync(m, sizeof(m), port, 1, 1000000000LL),
        ptp::build_follow_up(m, sizeof(m), port, 1, 1000000000LL),
        ptp::build_delay_resp(m, sizeof(m), port, 2, 12345, requester, 1000000000LL),
        ptp::build_announce(m, sizeof(m), port, 3, 1000000000LL),
    };
    uint8_t types[] = {0, 8, 9, 11};
    for (size_t i = 0; i < 4; ++i)
    {
        size_t length = 0;
        switch (i)
        {
        case 0: length = ptp::build_sync(m, sizeof(m), port, 1, 1000000000LL); break;
        case 1: length = ptp::build_follow_up(m, sizeof(m), port, 1, 1000000000LL); break;
        case 2: length = ptp::build_delay_resp(m, sizeof(m), port, 2, 12345, requester, 1000000000LL); break;
        default: length = ptp::build_announce(m, sizeof(m), port, 3, 1000000000LL); break;
        }
        CHECK(length == sizes[i] && length != 0 && (m[0] & 0x0f) == types[i] && m[1] == 0x12);
        CHECK(ptp_auth_append(m, sizeof(m), &length) && length == sizes[i] + PTP_AUTH_TLV_SIZE);
        CHECK(m[sizes[i] + 9] == 9); // key ID 9
        CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::valid);
        // Signing twice or a buffer without room fails
        size_t again = length;
        CHECK(!ptp_auth_append(m, sizeof(m), &again) && again == length);
    }
    size_t length = ptp::build_sync(m, sizeof(m), port, 4, 1000000000LL);
    CHECK(!ptp_auth_append(m, length + PTP_AUTH_TLV_SIZE - 1, &length) && length == ptp::SYNC_SIZE);
    m[1] = 0x02; // minorVersionPTP 0: not signed
    CHECK(!ptp_auth_append(m, sizeof(m), &length));
}

static void test_received()
{
    CHECK(configure(true, false, false));
    uint8_t m[44 + PTP_AUTH_TLV_SIZE];
    size_t length = delay_req(m);
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::unauthenticated);
    m[1] = 0x02;
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::unauthenticated); // unsigned PTPv2.0 follower
    m[1] = 0x12;
    CHECK(ptp_auth_append(m, sizeof(m), &length));
    // Each byte of the signed part and of the ICV
    for (size_t i = 0; i < length; ++i)
    {
        if (i == 2 || i == 3)
            continue; // messageLength: malformed, below
        m[i] ^= 0x10;
        const ptp_auth_result_t result = ptp_auth_verify(m, length);
        if (i == 1 || i == 44 || i == 45 || i == 46 || i == 47 || i == 48 || i == 49)
            CHECK(result == ptp_auth_result_t::malformed); // version, TLV type, length, spp, secParamIndicator
        else if (i >= 50 && i < 54)
            CHECK(result == ptp_auth_result_t::unknown_key);
        else
            CHECK(result == ptp_auth_result_t::invalid_mac);
        m[i] ^= 0x10;
    }
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::valid);
    // Key 9 signs, key 3 verifies as well (key ring)
    CHECK(configure(true, false, false, 9));
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::valid);
    // Key ID 0, lengths, other spp
    m[53] = 0;
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::unknown_key);
    m[53] = 3;
    CHECK(ptp_auth_verify(m, length - 1) == ptp_auth_result_t::malformed);
    CHECK(ptp_auth_verify(m, 33) == ptp_auth_result_t::malformed);
    CHECK(ptp_auth_verify(nullptr, 70) == ptp_auth_result_t::malformed);
    CHECK(configure(true, false, false, 3, 6));
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::malformed);
    // A type without a known body (Management)
    uint8_t other[44] = {13, 0x02, 0, 44};
    CHECK(ptp_auth_verify(other, 44) == ptp_auth_result_t::malformed);
}

static void test_policy()
{
    uint8_t m[44 + PTP_AUTH_TLV_SIZE];
    size_t length = delay_req(m);
    CHECK(configure(true, true, false));
    CHECK(ptp_auth_available() && ptp_auth_required());
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::authentication_required);
    CHECK(ptp_auth_append(m, sizeof(m), &length));
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::valid);

    // Off: nothing is signed, a signed message cannot be checked, required does not count
    CHECK(!configure(false, true, false));
    CHECK(!ptp_auth_available() && !ptp_auth_required());
    CHECK(ptp_auth_verify(m, length) == ptp_auth_result_t::unavailable);
    size_t plain = delay_req(m);
    CHECK(ptp_auth_verify(m, plain) == ptp_auth_result_t::unauthenticated);
    CHECK(!ptp_auth_append(m, sizeof(m), &plain) && plain == 44);

    // Active key not among the keys: fail closed while required
    CHECK(!configure(true, true, false, 77));
    CHECK(!ptp_auth_available() && ptp_auth_required());
    CHECK(ptp_auth_verify(m, plain) == ptp_auth_result_t::authentication_required);

    // Invalid keys are skipped
    const ptp_auth_key_input_t keys[] = {{0, KEY_3}, {70000, KEY_3}, {4, "short"}, {5, "0123456789abcdef0123456789abcde!"}, {6, KEY_3}, {6, KEY_9}};
    ptp_auth_config_t config;
    config.enabled = true;
    config.active_key_id = 6;
    CHECK(ptp_auth_configure(config, keys, 6));
    config.active_key_id = 5;
    CHECK(!ptp_auth_configure(config, keys, 6));

    // A failing hash: no signature, no valid verification, the message stays unsigned
    CHECK(configure(true, false, false));
    plain = delay_req(m);
    host_psa_forced_status() = -1;
    CHECK(!ptp_auth_append(m, sizeof(m), &plain) && plain == 44 && m[3] == 44);
    host_psa_forced_status() = PSA_SUCCESS;
    CHECK(ptp_auth_append(m, sizeof(m), &plain));
    host_psa_forced_status() = -1;
    CHECK(ptp_auth_verify(m, plain) == ptp_auth_result_t::invalid_mac);
    host_psa_forced_status() = PSA_SUCCESS;
    CHECK(ptp_auth_verify(m, plain) == ptp_auth_result_t::valid);
}

// HMAC-SHA256 itself (RFC 4231 test case 2) through the same PSA stub: SHA-256 of (K ^ opad) || SHA-256((K ^ ipad) || m)
static void test_hmac_construction()
{
    uint8_t pad[64];
    uint8_t inner[32], outer[32], expected[32];
    size_t n = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    memset(pad, 0x36, 64);
    for (size_t i = 0; i < 4; ++i)
        pad[i] ^= static_cast<uint8_t>("Jefe"[i]);
    const char text[] = "what do ya want for nothing?";
    CHECK(psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS && psa_hash_update(&op, pad, 64) == PSA_SUCCESS &&
          psa_hash_update(&op, reinterpret_cast<const uint8_t *>(text), strlen(text)) == PSA_SUCCESS &&
          psa_hash_finish(&op, inner, 32, &n) == PSA_SUCCESS);
    for (auto &b : pad)
        b ^= 0x36 ^ 0x5c;
    CHECK(psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS && psa_hash_update(&op, pad, 64) == PSA_SUCCESS &&
          psa_hash_update(&op, inner, 32) == PSA_SUCCESS && psa_hash_finish(&op, outer, 32, &n) == PSA_SUCCESS);
    hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", expected, 32);
    CHECK(memcmp(outer, expected, 32) == 0);
}

int main()
{
    CHECK(!ptp_auth_available() && !ptp_auth_required());
    test_hmac_construction();
    test_known_icv();
    test_master_messages();
    test_received();
    test_policy();
    printf("ptp_auth_test: %d checks passed\n", s_checks);
    return 0;
}
