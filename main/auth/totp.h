// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// TOTP, the second factor of the logins (pure, no ESP-IDF; HMAC-SHA1 and SHA-256 through PSA): RFC 6238 with the
// parameters every authenticator app takes (SHA-1, 6 digits, 30 s), RFC 4226 HOTP underneath, base32 (RFC 4648) for
// the key, the otpauth URI of the QR code, and the recovery codes.
//
// A code is accepted for the time step of now, one before or one after (clocks a little apart), and only for a step
// later than the last one used (each code once). Recovery codes: 10 per token, 10
// characters of the base32 alphabet ("abcde-fghij", 50 bits each), stored as a salted SHA-256 (the first 16 bytes),
// each usable once.
#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t TOTP_SECRET_SIZE = 20;      // 160 bits (RFC 4226 recommends this)
constexpr size_t TOTP_BASE32_SIZE = 33;      // 32 characters + NUL for 20 bytes
constexpr uint32_t TOTP_PERIOD_S = 30;
constexpr unsigned TOTP_DIGITS = 6;
constexpr size_t TOTP_RECOVERY_CODES = 10;
constexpr size_t TOTP_RECOVERY_TEXT_SIZE = 12; // "abcde-fghij" + NUL
constexpr size_t TOTP_RECOVERY_HASH = 16;
constexpr size_t TOTP_SALT_SIZE = 8;
constexpr size_t TOTP_URI_SIZE = 400; // the longest: host name 32 and user name 64 characters, all percent-encoded

// Base32 without padding (RFC 4648 alphabet, upper case). Returns the length written (out_size must hold it and NUL).
size_t totp_base32_encode(const uint8_t *data, size_t length, char *out, size_t out_size);
// Decodes base32 (upper or lower case, '=' padding, spaces and '-' ignored); false for any other character or more
// than out_size bytes. Used for keys typed by hand in tests and tools.
bool totp_base32_decode(const char *text, uint8_t *out, size_t out_size, size_t &out_length);

// HOTP (RFC 4226): the 6-digit code of the counter; false if HMAC-SHA1 failed
bool totp_hotp(const uint8_t *secret, size_t secret_length, uint64_t counter, uint32_t &code);
// The 6-digit code as text ("012345")
void totp_code_text(uint32_t code, char out[TOTP_DIGITS + 1]);

// Checks a code typed by the user (6 digits, spaces allowed) at unix time now_s. On success *step is the time step
// it matched (> last_step). false: wrong, not 6 digits, already used, or the time is before 1970 + one period.
bool totp_verify(const uint8_t *secret, size_t secret_length, int64_t now_s, const char *code, uint64_t last_step, uint64_t &step);

// The otpauth URI of the QR code: otpauth://totp/<issuer>:<user>?secret=<base32>&issuer=<issuer>&algorithm=SHA1&digits=6&period=30
// (issuer and user percent-encoded). false if it does not fit.
bool totp_uri(const char *issuer, const char *user, const uint8_t *secret, size_t secret_length, char *out, size_t out_size);

// Recovery codes: the text of 50 random bits (random: 7 bytes, the top 6 bits are dropped)
void totp_recovery_text(const uint8_t random[7], char out[TOTP_RECOVERY_TEXT_SIZE]);
// Normalises a code typed by the user (lower case, without '-' and spaces) into out (10 characters + NUL); false if
// it is not 10 characters of the alphabet
bool totp_recovery_normalise(const char *text, char out[11]);
// The stored hash of a normalised code (SHA-256 of salt and code, cut to TOTP_RECOVERY_HASH bytes)
bool totp_recovery_hash(const uint8_t salt[TOTP_SALT_SIZE], const char normalised[11], uint8_t out[TOTP_RECOVERY_HASH]);
// Whether text is a code of the list (constant time over the list): its index, or -1. An all-zero hash is a used code.
int totp_recovery_find(const uint8_t salt[TOTP_SALT_SIZE], const uint8_t hashes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_HASH], const char *text);
