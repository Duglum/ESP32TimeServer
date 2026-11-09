// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The TOTP tokens (NVS namespace "totp", one blob per token "t0" ... "t8"): the second factor of the logins of the web
// page, one token per user name (the local user "admin", RADIUS users). Each token has its key (20 random bytes, made
// on the device), the time step last used (each code once) and 10 recovery codes (salted hashes; each once). Not in
// the configuration file; erased by Reset to defaults. Thread safe.
//
// The time server has 24 kB of NVS, and its settings blob (about 7 kB) is written again by every Save and Apply with a
// restart: a new token is only stored while NVS keeps room for that (totp_store_init's reserve).
#pragma once

#include <cstddef>
#include <cstdint>

#include "auth/totp.h"
#include "esp_err.h"

constexpr char TOTP_NVS_NAMESPACE[] = "totp";
constexpr size_t TOTP_TOKENS_MAX = 9;   // the local user and 8 more (about 300 bytes of NVS each)
constexpr size_t TOTP_USER_SIZE = 65;   // 64 bytes + NUL (a RADIUS user name)

struct totp_token_info_t
{
    char user[TOTP_USER_SIZE];
    int64_t created_s;   // unix time (0: the time was not set)
    int64_t last_used_s; // 0: never
    unsigned recovery_left;
};

// Loads the tokens (after nvs_flash_init). nvs_reserve_bytes: what NVS must keep free after a new token (the other
// data that is written again, e.g. the settings)
void totp_store_init(size_t nvs_reserve_bytes);
bool totp_store_has(const char *user);
size_t totp_store_list(totp_token_info_t *out, size_t capacity);
size_t totp_store_count();

// Stores the token of user (replacing one the user had) with 10 new recovery codes, given back in codes. Errors:
// ESP_ERR_NO_MEM all TOTP_TOKENS_MAX in use, ESP_ERR_NVS_NOT_ENOUGH_SPACE NVS is full (or would keep less than the
// reserve), ESP_ERR_INVALID_ARG a user name of 0 or more than 64 bytes.
esp_err_t totp_store_add(const char *user, const uint8_t secret[TOTP_SECRET_SIZE], char codes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_TEXT_SIZE]);
// Deletes the token of user; false if there is none
bool totp_store_remove(const char *user);

enum class totp_check_t : uint8_t
{
    code,      // a right TOTP code (now used)
    recovery,  // a right recovery code (now used)
    wrong,     // wrong, already used, or a TOTP code while the time is not set
    no_token,  // the user has no token
};
// Checks what the user typed: a 6-digit TOTP code (only while the clock is set: time after 2026-01-01) or a recovery
// code. A right one is marked used at once (NVS). The work is the same for a user without a token.
totp_check_t totp_store_check(const char *user, const char *typed);

// The clock is set (GNSS): TOTP codes can be checked
bool totp_store_time_ok();
