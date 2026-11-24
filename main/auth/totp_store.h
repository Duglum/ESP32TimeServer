// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The TOTP tokens (NVS namespace "totp", one blob per token "t0" ... "t8"): the second factor of the logins of the web
// page, one token per user name (the local user "admin", RADIUS users). Each token has its key (20 random bytes, made
// on the device), the time step last used (each code once) and 10 recovery codes (salted hashes; each once). Not in
// the configuration file; erased by Reset to defaults. Thread safe.
//
// The settings blob (about 7 kB) is written again by every Save and Apply with a restart: a new token is only stored
// while NVS keeps room for that (totp_store_init's reserve). The nvs partition has 256 kB; a board that still has the
// partition table before 2026-11 has 24 kB (README, "Partition table and NVS"), where this reserve limits the tokens.
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

// 10 new recovery codes (random) for totp_store_add(). Made first, so that the answer that shows them can be built
// before the token is stored (web_server.cpp).
void totp_store_new_codes(char codes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_TEXT_SIZE]);
// Stores the token of user (replacing one the user had) with the recovery codes of totp_store_new_codes() (stored as
// salted hashes). step: the time step of the code that confirmed the token (totp_verify), stored as the last one
// used, so that this code and the ones before it are refused afterwards as after a login. Errors: ESP_ERR_NO_MEM all
// TOTP_TOKENS_MAX in use, ESP_ERR_NVS_NOT_ENOUGH_SPACE NVS is full (or would keep less than the reserve),
// ESP_ERR_INVALID_ARG a user name of 0 or more than 64 bytes, ESP_ERR_INVALID_STATE the stored token of this user was
// lost (totp_store_lost).
esp_err_t totp_store_add(const char *user, const uint8_t secret[TOTP_SECRET_SIZE], uint64_t step,
                         const char codes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_TEXT_SIZE]);
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

// The stored token of user was lost (NVS: its presence mark without a valid record, see sys/record_mark.h), found at the
// start: the user is refused, totp_store_add() refuses a new token for it (ESP_ERR_INVALID_STATE), and its slot is not
// given to another user. Ends only with an erase of NVS (USB) or Reset to defaults.
bool totp_store_lost(const char *user);
// The number of lost tokens (0: none)
size_t totp_store_lost_count();
