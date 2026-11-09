// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//

#include "auth/totp.h"

#include <cstdio>
#include <cstring>

extern "C"
{
#include "psa/crypto.h"
}

namespace
{
    constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    constexpr char RECOVERY_ALPHABET[] = "abcdefghijklmnopqrstuvwxyz234567";

    int base32_value(char c)
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a';
        if (c >= '2' && c <= '7')
            return c - '2' + 26;
        return -1;
    }

    bool hmac_sha1(const uint8_t *key, size_t key_length, const uint8_t *data, size_t length, uint8_t out[20])
    {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_1));
        psa_key_id_t id = 0;
        if (key_length == 0 || psa_import_key(&attributes, key, key_length, &id) != PSA_SUCCESS)
            return false;
        size_t out_length = 0;
        const bool ok = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_1), data, length, out, 20, &out_length) == PSA_SUCCESS && out_length == 20;
        psa_destroy_key(id);
        return ok;
    }

    bool equal(const uint8_t *a, const uint8_t *b, size_t length)
    {
        uint8_t diff = 0;
        for (size_t i = 0; i < length; ++i)
            diff |= static_cast<uint8_t>(a[i] ^ b[i]);
        return diff == 0;
    }

    // Appends text percent-encoded (RFC 3986 unreserved characters as they are)
    bool append_encoded(char *out, size_t out_size, size_t &at, const char *text)
    {
        static constexpr char HEX[] = "0123456789ABCDEF";
        for (const char *p = text; *p != '\0'; ++p)
        {
            const unsigned char c = static_cast<unsigned char>(*p);
            const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
            if (at + (plain ? 1 : 3) >= out_size)
                return false;
            if (plain)
                out[at++] = static_cast<char>(c);
            else
            {
                out[at++] = '%';
                out[at++] = HEX[c >> 4];
                out[at++] = HEX[c & 0x0F];
            }
        }
        out[at] = '\0';
        return true;
    }

    bool append(char *out, size_t out_size, size_t &at, const char *text)
    {
        const size_t n = strlen(text);
        if (at + n >= out_size)
            return false;
        memcpy(out + at, text, n + 1);
        at += n;
        return true;
    }
} // namespace

size_t totp_base32_encode(const uint8_t *data, size_t length, char *out, size_t out_size)
{
    const size_t chars = (length * 8 + 4) / 5;
    if (out_size < chars + 1)
    {
        if (out_size > 0)
            out[0] = '\0';
        return 0;
    }
    uint32_t buffer = 0;
    int bits = 0;
    size_t at = 0;
    for (size_t i = 0; i < length; ++i)
    {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5)
        {
            out[at++] = ALPHABET[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits > 0)
        out[at++] = ALPHABET[(buffer << (5 - bits)) & 31];
    out[at] = '\0';
    return at;
}

bool totp_base32_decode(const char *text, uint8_t *out, size_t out_size, size_t &out_length)
{
    out_length = 0;
    uint32_t buffer = 0;
    int bits = 0;
    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p == '=' || *p == ' ' || *p == '-')
            continue;
        const int v = base32_value(*p);
        if (v < 0)
            return false;
        buffer = (buffer << 5) | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8)
        {
            if (out_length == out_size)
                return false;
            out[out_length++] = static_cast<uint8_t>(buffer >> (bits - 8));
            bits -= 8;
        }
    }
    return true;
}

bool totp_hotp(const uint8_t *secret, size_t secret_length, uint64_t counter, uint32_t &code)
{
    uint8_t message[8];
    for (int i = 7; i >= 0; --i)
    {
        message[i] = static_cast<uint8_t>(counter);
        counter >>= 8;
    }
    uint8_t mac[20];
    if (!hmac_sha1(secret, secret_length, message, sizeof(message), mac))
        return false;
    const unsigned offset = mac[19] & 0x0F; // dynamic truncation (RFC 4226 5.3)
    const uint32_t binary = (static_cast<uint32_t>(mac[offset] & 0x7F) << 24) | (static_cast<uint32_t>(mac[offset + 1]) << 16) |
                            (static_cast<uint32_t>(mac[offset + 2]) << 8) | mac[offset + 3];
    code = binary % 1000000u;
    memset(mac, 0, sizeof(mac));
    return true;
}

void totp_code_text(uint32_t code, char out[TOTP_DIGITS + 1])
{
    snprintf(out, TOTP_DIGITS + 1, "%06u", static_cast<unsigned>(code % 1000000u));
}

bool totp_verify(const uint8_t *secret, size_t secret_length, int64_t now_s, const char *code, uint64_t last_step, uint64_t &step)
{
    step = 0;
    if (code == nullptr || now_s < static_cast<int64_t>(TOTP_PERIOD_S))
        return false;
    char digits[TOTP_DIGITS + 1];
    size_t n = 0;
    for (const char *p = code; *p != '\0'; ++p)
    {
        if (*p == ' ')
            continue;
        if (*p < '0' || *p > '9' || n == TOTP_DIGITS)
            return false;
        digits[n++] = *p;
    }
    if (n != TOTP_DIGITS)
        return false;
    digits[n] = '\0';
    const uint64_t now_step = static_cast<uint64_t>(now_s) / TOTP_PERIOD_S;
    bool found = false;
    // every step of the window is computed and compared, so the time does not tell which one matched
    for (uint64_t s = now_step - 1; s <= now_step + 1; ++s)
    {
        uint32_t value = 0;
        if (!totp_hotp(secret, secret_length, s, value))
            return false;
        char text[TOTP_DIGITS + 1];
        totp_code_text(value, text);
        if (equal(reinterpret_cast<const uint8_t *>(text), reinterpret_cast<const uint8_t *>(digits), TOTP_DIGITS) && s > last_step && !found)
        {
            found = true;
            step = s;
        }
    }
    return found;
}

bool totp_uri(const char *issuer, const char *user, const uint8_t *secret, size_t secret_length, char *out, size_t out_size)
{
    char key[TOTP_BASE32_SIZE];
    if (secret_length > TOTP_SECRET_SIZE || totp_base32_encode(secret, secret_length, key, sizeof(key)) == 0)
        return false;
    size_t at = 0;
    out[0] = '\0';
    const bool ok = append(out, out_size, at, "otpauth://totp/") && append_encoded(out, out_size, at, issuer) && append(out, out_size, at, ":") &&
                    append_encoded(out, out_size, at, user) && append(out, out_size, at, "?secret=") && append(out, out_size, at, key) &&
                    append(out, out_size, at, "&issuer=") && append_encoded(out, out_size, at, issuer) &&
                    append(out, out_size, at, "&algorithm=SHA1&digits=6&period=30");
    memset(key, 0, sizeof(key));
    if (!ok && out_size > 0)
        out[0] = '\0';
    return ok;
}

void totp_recovery_text(const uint8_t random[7], char out[TOTP_RECOVERY_TEXT_SIZE])
{
    uint64_t bits = 0;
    for (int i = 0; i < 7; ++i)
        bits = (bits << 8) | random[i];
    size_t at = 0;
    for (int i = 0; i < 10; ++i)
    {
        if (i == 5)
            out[at++] = '-';
        out[at++] = RECOVERY_ALPHABET[(bits >> (5 * (9 - i))) & 31];
    }
    out[at] = '\0';
}

bool totp_recovery_normalise(const char *text, char out[11])
{
    size_t n = 0;
    for (const char *p = text; *p != '\0'; ++p)
    {
        if (*p == '-' || *p == ' ')
            continue;
        const char c = *p >= 'A' && *p <= 'Z' ? static_cast<char>(*p - 'A' + 'a') : *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7')) || n == 10)
            return false;
        out[n++] = c;
    }
    out[n] = '\0';
    return n == 10;
}

bool totp_recovery_hash(const uint8_t salt[TOTP_SALT_SIZE], const char normalised[11], uint8_t out[TOTP_RECOVERY_HASH])
{
    uint8_t input[TOTP_SALT_SIZE + 10];
    memcpy(input, salt, TOTP_SALT_SIZE);
    memcpy(input + TOTP_SALT_SIZE, normalised, 10);
    uint8_t hash[32];
    size_t length = 0;
    const bool ok = psa_hash_compute(PSA_ALG_SHA_256, input, sizeof(input), hash, sizeof(hash), &length) == PSA_SUCCESS && length == 32;
    if (ok)
        memcpy(out, hash, TOTP_RECOVERY_HASH);
    memset(input, 0, sizeof(input));
    return ok;
}

int totp_recovery_find(const uint8_t salt[TOTP_SALT_SIZE], const uint8_t hashes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_HASH], const char *text)
{
    char normalised[11];
    uint8_t hash[TOTP_RECOVERY_HASH];
    if (text == nullptr || !totp_recovery_normalise(text, normalised) || !totp_recovery_hash(salt, normalised, hash))
        return -1;
    static constexpr uint8_t USED[TOTP_RECOVERY_HASH] = {};
    int found = -1;
    for (size_t i = 0; i < TOTP_RECOVERY_CODES; ++i)
        if (equal(hashes[i], hash, TOTP_RECOVERY_HASH) && !equal(hashes[i], USED, TOTP_RECOVERY_HASH) && found < 0)
            found = static_cast<int>(i);
    return found;
}
