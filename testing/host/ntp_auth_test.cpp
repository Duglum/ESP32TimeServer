// ESP32 Time Server - host test of the NTP symmetric-key authentication (main/custom/ntp_auth.cpp, see README.md):
// the MAC (SHA-256 of key and packet, 160 bit), the results of ntp_auth_verify_request() in the mixed and in the
// authenticated-only policy (web setting ntp_auth_required), and the key set of ntp_auth_configure().
#undef NDEBUG
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "ntp_auth.h"
#include "psa/crypto.h"

static int s_checks = 0;
#define CHECK(condition)  \
    do                    \
    {                     \
        assert(condition); \
        ++s_checks;       \
    } while (0)

static const char KEY_1[] = "0123456789abcdefABCDEF0123456789";
static const char KEY_7[] = "ZYXWVUTSRQPONMLKJIHGFEDCBA987654";

static void sha256(const void *data, size_t length, uint8_t out[32])
{
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    size_t out_length = 0;
    CHECK(psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS);
    CHECK(psa_hash_update(&operation, static_cast<const uint8_t *>(data), length) == PSA_SUCCESS);
    CHECK(psa_hash_finish(&operation, out, 32, &out_length) == PSA_SUCCESS && out_length == 32);
}

static void hex(const char *text, uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        unsigned value = 0;
        sscanf(text + i * 2, "%2x", &value);
        out[i] = static_cast<uint8_t>(value);
    }
}

// The SHA-256 of the stub against FIPS 180-4 examples (also over block borders)
static void test_sha256_stub()
{
    uint8_t digest[32], expected[32];
    sha256("abc", 3, digest);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", expected, 32);
    CHECK(memcmp(digest, expected, 32) == 0);
    const char two_blocks[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two_blocks, strlen(two_blocks), digest);
    hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", expected, 32);
    CHECK(memcmp(digest, expected, 32) == 0);
    sha256("", 0, digest);
    hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", expected, 32);
    CHECK(memcmp(digest, expected, 32) == 0);
}

static size_t request(uint8_t *packet, uint8_t version)
{
    memset(packet, 0, 48 + NTP_AUTH_TRAILER_SIZE);
    packet[0] = static_cast<uint8_t>((version << 3) | 3);
    return 48;
}

static size_t signed_request(uint8_t *packet, uint32_t key_id)
{
    size_t length = request(packet, 4);
    CHECK(ntp_auth_append_response(packet, 48 + NTP_AUTH_TRAILER_SIZE, &length, key_id));
    CHECK(length == 48 + NTP_AUTH_TRAILER_SIZE);
    return length;
}

static void configure(bool enabled, bool required, size_t key_count)
{
    const ntp_auth_key_input_t keys[] = {{1, KEY_1}, {7, KEY_7}};
    ntp_auth_configure(enabled, required, keys, key_count);
}

static ntp_auth_result_t verify(const uint8_t *packet, size_t length, uint8_t version, uint32_t *key_id = nullptr)
{
    uint32_t id = 0xFFFFFFFF;
    const ntp_auth_result_t result = ntp_auth_verify_request(packet, length, version, &id);
    if (key_id != nullptr)
        *key_id = id;
    if (result != ntp_auth_result_t::valid)
        CHECK(id == 0);
    return result;
}

// MAC of a request: SHA-256(key || packet), first 20 bytes, after the key ID (computed with Python hashlib)
static void test_known_mac()
{
    configure(true, false, 2);
    uint8_t packet[48 + NTP_AUTH_TRAILER_SIZE];
    size_t length = request(packet, 4);
    CHECK(ntp_auth_append_response(packet, sizeof(packet), &length, 1));
    uint8_t expected[NTP_AUTH_TRAILER_SIZE] = {0, 0, 0, 1};
    hex("5bd0e06ca7564e3d773e23db855a210b7e309ce0", expected + 4, 20);
    CHECK(memcmp(packet + 48, expected, sizeof(expected)) == 0);
    uint32_t key_id = 0;
    CHECK(verify(packet, length, 4, &key_id) == ntp_auth_result_t::valid && key_id == 1);
    // Wrong buffer size or a packet that already has a trailer
    size_t again = length;
    CHECK(!ntp_auth_append_response(packet, sizeof(packet), &again, 1));
    size_t short_length = 48;
    CHECK(!ntp_auth_append_response(packet, 48 + NTP_AUTH_TRAILER_SIZE - 1, &short_length, 1));
    CHECK(!ntp_auth_append_response(packet, sizeof(packet), &short_length, 99)); // unknown key
}

// Mixed (default): unsigned NTPv3 and NTPv4 are answered, signed ones need a valid MAC
static void test_mixed()
{
    configure(true, false, 2);
    CHECK(ntp_auth_available() && !ntp_auth_required() && ntp_auth_key_count() == 2);
    uint8_t packet[48 + NTP_AUTH_TRAILER_SIZE];
    CHECK(verify(packet, request(packet, 4), 4) == ntp_auth_result_t::unauthenticated);
    CHECK(verify(packet, request(packet, 3), 3) == ntp_auth_result_t::unauthenticated);

    uint32_t key_id = 0;
    size_t length = signed_request(packet, 7);
    CHECK(verify(packet, length, 4, &key_id) == ntp_auth_result_t::valid && key_id == 7);
    // A changed byte anywhere in the packet or the MAC
    for (size_t index : {size_t(0), size_t(40), size_t(47), size_t(52), length - 1})
    {
        packet[index] ^= 0x01;
        CHECK(verify(packet, length, 4) == ntp_auth_result_t::invalid_mac);
        packet[index] ^= 0x01;
    }
    // Unknown key ID
    packet[51] = 9;
    CHECK(verify(packet, length, 4) == ntp_auth_result_t::unknown_key);
    // NTPv3 with a trailer, wrong lengths
    length = signed_request(packet, 1);
    CHECK(verify(packet, length, 3) == ntp_auth_result_t::malformed);
    CHECK(verify(packet, length - 1, 4) == ntp_auth_result_t::malformed);
    CHECK(verify(packet, 48 + 4, 4) == ntp_auth_result_t::malformed); // crypto-NAK shape (key ID only)
    CHECK(verify(packet, 47, 4) == ntp_auth_result_t::malformed);
    CHECK(verify(nullptr, 48, 4) == ntp_auth_result_t::malformed);
}

// Authentication required: every unsigned request (NTPv3 is always unsigned) is refused, signed ones as before
static void test_required()
{
    configure(true, true, 2);
    CHECK(ntp_auth_available() && ntp_auth_required());
    uint8_t packet[48 + NTP_AUTH_TRAILER_SIZE];
    CHECK(verify(packet, request(packet, 4), 4) == ntp_auth_result_t::authentication_required);
    CHECK(verify(packet, request(packet, 3), 3) == ntp_auth_result_t::authentication_required);
    uint32_t key_id = 0;
    size_t length = signed_request(packet, 1);
    CHECK(verify(packet, length, 4, &key_id) == ntp_auth_result_t::valid && key_id == 1);
    packet[60] ^= 0x80;
    CHECK(verify(packet, length, 4) == ntp_auth_result_t::invalid_mac);
    packet[60] ^= 0x80;
    packet[50] = 1;
    CHECK(verify(packet, length, 4) == ntp_auth_result_t::unknown_key);
    CHECK(verify(packet, 48 + 4, 4) == ntp_auth_result_t::malformed);

    // Back to mixed: unsigned requests are answered again at once
    configure(true, false, 2);
    CHECK(!ntp_auth_required());
    CHECK(verify(packet, request(packet, 4), 4) == ntp_auth_result_t::unauthenticated);
}

// required only counts with authentication on; without a usable key it stays in force (fail closed)
static void test_policy_edges()
{
    uint8_t packet[48 + NTP_AUTH_TRAILER_SIZE];
    configure(false, true, 2);
    CHECK(!ntp_auth_available() && !ntp_auth_required() && ntp_auth_key_count() == 0);
    CHECK(verify(packet, request(packet, 4), 4) == ntp_auth_result_t::unauthenticated);
    packet[0] = 0x23;
    memset(packet + 48, 0, NTP_AUTH_TRAILER_SIZE);
    CHECK(verify(packet, 48 + NTP_AUTH_TRAILER_SIZE, 4) == ntp_auth_result_t::unavailable);

    configure(true, true, 0); // the web settings refuse this; the module must not answer unsigned requests then
    CHECK(!ntp_auth_available() && ntp_auth_required());
    CHECK(verify(packet, request(packet, 4), 4) == ntp_auth_result_t::authentication_required);
    CHECK(verify(packet, 48 + NTP_AUTH_TRAILER_SIZE, 4) == ntp_auth_result_t::unavailable);

    // Invalid keys of the settings are skipped (ID 0, too short, not alphanumeric, duplicate ID)
    const ntp_auth_key_input_t keys[] = {{0, KEY_1}, {2, "short"}, {3, "0123456789abcdef0123456789abcde!"}, {4, KEY_1}, {4, KEY_7}};
    CHECK(ntp_auth_configure(true, false, keys, 5));
    CHECK(ntp_auth_key_count() == 1);
    uint32_t key_id = 0;
    char text[NTP_AUTH_MAX_KEY_SIZE * 2 + 1];
    CHECK(ntp_auth_key_hex(0, &key_id, text, sizeof(text)) && key_id == 4 && strlen(text) == 64);
    CHECK(ntp_auth_default_key_ascii(text, sizeof(text)) && strcmp(text, KEY_1) == 0);

    // A failing hash: the request does not verify, no reply is signed
    configure(true, true, 2);
    size_t length = signed_request(packet, 1);
    host_psa_forced_status() = -1;
    CHECK(verify(packet, length, 4) == ntp_auth_result_t::invalid_mac);
    size_t reply_length = 48;
    CHECK(!ntp_auth_append_response(packet, sizeof(packet), &reply_length, 1) && reply_length == 48);
    host_psa_forced_status() = PSA_SUCCESS;
    CHECK(verify(packet, length, 4) == ntp_auth_result_t::valid);
}

int main()
{
    CHECK(ntp_auth_initialize());
    test_sha256_stub();
    // Before the first ntp_auth_configure(): off, unsigned requests are answered
    uint8_t packet[48 + NTP_AUTH_TRAILER_SIZE];
    CHECK(!ntp_auth_available() && !ntp_auth_required());
    CHECK(verify(packet, request(packet, 4), 4) == ntp_auth_result_t::unauthenticated);
    test_known_mac();
    test_mixed();
    test_required();
    test_policy_edges();
    printf("ntp_auth_test: %d checks passed\n", s_checks);
    return 0;
}
