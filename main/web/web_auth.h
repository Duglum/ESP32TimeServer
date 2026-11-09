// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Authentication of the web admin page: password hash (PBKDF2-HMAC-SHA256, NVS namespace "webauth"), sessions,
// CSRF tokens, login throttling and the source address allow-list.
// All functions except web_auth_init() are called from the web server task only (no locking).

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct sockaddr_storage;

static constexpr size_t WEB_SESSION_TOKEN_HEX_LEN = 64; // 256 bit
static constexpr size_t WEB_CSRF_TOKEN_HEX_LEN = 64;
static constexpr size_t WEB_PASSWORD_MIN_LEN = 12;
static constexpr size_t WEB_PASSWORD_MAX_LEN = 128;
static constexpr size_t WEB_ALLOW_LIST_MAX = 8;
static constexpr int64_t WEB_SESSION_IDLE_TIMEOUT_US = 15LL * 60LL * 1000000LL;
static constexpr int64_t WEB_SESSION_MAX_AGE_US = 12LL * 3600LL * 1000000LL;
static constexpr size_t WEB_SESSION_USER_SIZE = 65; // the user name of the login (the local user or a RADIUS user)

struct web_allow_entry_t
{
    uint8_t family; // 4 or 6
    uint8_t prefix;
    uint8_t address[16];
};

// Loads the password hash and the allow-list (call once after nvs_flash_init)
void web_auth_init();

// true while no password was set on the page: the default "password" is in use
bool web_auth_default_password_active();

// ---- Login throttling (per source address and global) ----
// Returns 0 if a login attempt from this address may be checked now, otherwise the seconds to wait
uint32_t web_auth_login_wait_seconds(const sockaddr_storage &source);
// Verifies the password (constant time). A failure counts for the throttling; a success does not forget the failures
// of the source: the caller records it with web_auth_record_success() once the whole check is done (the session of a
// login, after a second factor; a scripted upload with its code), so a known password cannot reset the counter
// between guesses of a TOTP code. The password change and the reset to defaults record no success.
bool web_auth_verify_current_password(const sockaddr_storage &source, const char *password, size_t length);
// Records a failed check without a password to verify (unreadable credentials of a scripted firmware upload, a login
// refused by RADIUS, a wrong TOTP code)
void web_auth_record_failed_check(const sockaddr_storage &source);
// Records a complete successful check (the session of a login, local or RADIUS, after its second factor; a scripted
// upload with its password and code): the failures of the source are forgotten
void web_auth_record_success(const sockaddr_storage &source);

// ---- Password change: stored immediately, all other sessions are closed ----
esp_err_t web_auth_set_password(const char *password, size_t length, const char *keep_session_token);

// ---- Sessions ----
// Creates a session; token and csrf receive hex strings (WEB_*_HEX_LEN + 1 bytes). user: the user name of the login
// (nullptr: the local user "admin"), radius: RADIUS accepted it, totp: it passed the second factor as well.
bool web_auth_create_session(char *token, char *csrf, const char *user = nullptr, bool radius = false, bool totp = false);
// Looks up a session by its cookie token and refreshes its idle timer. csrf (optional) receives the CSRF token.
bool web_auth_session_valid(const char *token, char *csrf);
// Checks the CSRF token of a session (constant time)
bool web_auth_csrf_valid(const char *token, const char *csrf);
void web_auth_end_session(const char *token);
// The login of the session of token (user name, by RADIUS, with the second factor); false if there is none. The idle
// timer is not refreshed.
bool web_auth_session_login(const char *token, char *user, size_t user_size, bool &radius, bool &totp);
// A random id of the session of token (0 = none; not derived from the token), without refreshing its idle timer
uint32_t web_auth_session_id(const char *token);
void web_auth_end_all_sessions();

// ---- Allow-list ----
size_t web_auth_allow_list(web_allow_entry_t *entries, size_t capacity);
// Replaces the allow-list and stores it in NVS. Refused (ESP_ERR_INVALID_STATE) if the client would be locked out.
esp_err_t web_auth_set_allow_list(const web_allow_entry_t *entries, size_t count, const sockaddr_storage &client);
bool web_auth_source_allowed(const sockaddr_storage &source);
bool web_auth_parse_cidr(const char *text, web_allow_entry_t &entry);
void web_auth_format_cidr(const web_allow_entry_t &entry, char *out, size_t out_size);

// Hex encoding of random bytes from the hardware RNG
void web_random_hex(char *out, size_t bytes);
