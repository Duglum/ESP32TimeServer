// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "web_auth.h"
#include "web_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ESP32TimeServerSettings.h"

extern "C"
{
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "psa/crypto.h"
}

namespace
{
    const char *TAG = "web_auth";

    constexpr char NVS_NAMESPACE[] = "webauth";
    constexpr char NVS_KEY_PASSWORD[] = "pw";
    constexpr char NVS_KEY_ALLOW[] = "allow";
    constexpr uint32_t PASSWORD_MAGIC = 0x57505744u; // "WPWD"
    constexpr uint32_t ALLOW_MAGIC = 0x57414C4Cu;    // "WALL"
    // PBKDF2 iterations: about 0.3 s per check on the ESP32-P4. Stored with the hash, so it can be raised later.
    constexpr uint32_t PBKDF2_ITERATIONS = 10000;
    constexpr size_t SALT_SIZE = 16;
    constexpr size_t HASH_SIZE = 32;
    constexpr char FACTORY_PASSWORD[] = "password";

    struct password_record_t
    {
        uint32_t magic;
        uint32_t iterations;
        uint8_t salt[SALT_SIZE];
        uint8_t hash[HASH_SIZE];
        uint32_t crc;
    };

    struct allow_record_t
    {
        uint32_t magic;
        uint32_t count;
        web_allow_entry_t entries[WEB_ALLOW_LIST_MAX];
        uint32_t crc;
    };

    struct session_t
    {
        bool used;
        uint8_t token_hash[HASH_SIZE]; // SHA-256 of the cookie token (the token itself is not kept)
        char csrf[WEB_CSRF_TOKEN_HEX_LEN + 1];
        int64_t created_us;
        int64_t last_used_us;
    };

    struct throttle_entry_t
    {
        uint8_t address[16]; // IPv4 as IPv4-mapped IPv6
        uint32_t failures;
        int64_t locked_until_us;
        int64_t last_failure_us;
    };

    constexpr size_t MAX_SESSIONS = 4;
    constexpr size_t THROTTLE_ENTRIES = 16;
    constexpr uint32_t FAILURES_BEFORE_LOCKOUT = 5;
    constexpr int64_t FIRST_LOCKOUT_US = 30LL * 1000000LL;
    constexpr int64_t MAX_LOCKOUT_US = 15LL * 60LL * 1000000LL;
    constexpr int64_t FAILURE_MEMORY_US = 60LL * 60LL * 1000000LL; // failures are forgotten after an hour without one
    constexpr uint32_t GLOBAL_FAILURES_PER_MINUTE = 20;

    password_record_t s_password{};
    bool s_password_stored = false;
    session_t s_sessions[MAX_SESSIONS]{};
    throttle_entry_t s_throttle[THROTTLE_ENTRIES]{};
    web_allow_entry_t s_allow[WEB_ALLOW_LIST_MAX]{};
    size_t s_allow_count = 0;

    // Global throttling: failures counted per minute window
    int64_t s_global_window_start_us = 0;
    uint32_t s_global_window_failures = 0;
    int64_t s_global_locked_until_us = 0;
    int64_t s_global_lockout_us = 0;
    int64_t s_global_last_lockout_us = 0;

    uint32_t crc_of(const void *data, size_t length)
    {
        return esp_rom_crc32_le(0, static_cast<const uint8_t *>(data), length);
    }

    bool sha256(const void *data, size_t length, uint8_t out[HASH_SIZE])
    {
        size_t out_length = 0;
        return psa_hash_compute(PSA_ALG_SHA_256, static_cast<const uint8_t *>(data), length, out, HASH_SIZE, &out_length) == PSA_SUCCESS &&
               out_length == HASH_SIZE;
    }

    // PBKDF2-HMAC-SHA256 (RFC 8018), one output block of 32 bytes
    bool pbkdf2_sha256(const char *password, size_t password_length, const uint8_t *salt, size_t salt_length,
                       uint32_t iterations, uint8_t out[HASH_SIZE])
    {
        if (password_length == 0 || iterations == 0)
            return false;
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
        psa_key_id_t key = 0;
        if (psa_import_key(&attributes, reinterpret_cast<const uint8_t *>(password), password_length, &key) != PSA_SUCCESS)
            return false;

        uint8_t block[SALT_SIZE + 4] = {};
        uint8_t u[HASH_SIZE] = {};
        size_t mac_length = 0;
        bool ok = salt_length <= SALT_SIZE;
        if (ok)
        {
            memcpy(block, salt, salt_length);
            block[salt_length + 3] = 1; // INT(1), big endian
            ok = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), block, salt_length + 4, u, sizeof(u), &mac_length) == PSA_SUCCESS &&
                 mac_length == HASH_SIZE;
        }
        if (ok)
            memcpy(out, u, HASH_SIZE);
        for (uint32_t i = 1; ok && i < iterations; ++i)
        {
            ok = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), u, sizeof(u), u, sizeof(u), &mac_length) == PSA_SUCCESS &&
                 mac_length == HASH_SIZE;
            for (size_t k = 0; ok && k < HASH_SIZE; ++k)
                out[k] ^= u[k];
        }
        psa_destroy_key(key);
        memset(u, 0, sizeof(u));
        return ok;
    }

    void to_mapped(const sockaddr_storage &source, uint8_t out[16])
    {
        memset(out, 0, 16);
        if (source.ss_family == AF_INET)
        {
            out[10] = 0xFF;
            out[11] = 0xFF;
            memcpy(out + 12, &reinterpret_cast<const sockaddr_in *>(&source)->sin_addr, 4);
        }
        else if (source.ss_family == AF_INET6)
            memcpy(out, &reinterpret_cast<const sockaddr_in6 *>(&source)->sin6_addr, 16);
    }

    bool is_v4_mapped(const uint8_t address[16])
    {
        static constexpr uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        return memcmp(address, prefix, 12) == 0;
    }

    throttle_entry_t *find_throttle(const uint8_t address[16], bool create)
    {
        throttle_entry_t *oldest = &s_throttle[0];
        for (throttle_entry_t &entry : s_throttle)
        {
            if (entry.failures != 0 && memcmp(entry.address, address, 16) == 0)
                return &entry;
            if (entry.failures == 0)
                oldest = &entry;
            else if (oldest->failures != 0 && entry.last_failure_us < oldest->last_failure_us)
                oldest = &entry;
        }
        if (!create)
            return nullptr;
        *oldest = throttle_entry_t{};
        memcpy(oldest->address, address, 16);
        return oldest;
    }

    void forget_old_failures(int64_t now_us)
    {
        for (throttle_entry_t &entry : s_throttle)
            if (entry.failures != 0 && now_us - entry.last_failure_us > FAILURE_MEMORY_US && now_us >= entry.locked_until_us)
                entry = throttle_entry_t{};
        if (s_global_lockout_us != 0 && now_us - s_global_last_lockout_us > FAILURE_MEMORY_US)
            s_global_lockout_us = 0;
    }

    void record_failure(const sockaddr_storage &source)
    {
        const int64_t now_us = esp_timer_get_time();
        uint8_t address[16];
        to_mapped(source, address);
        throttle_entry_t *entry = find_throttle(address, true);
        entry->failures++;
        entry->last_failure_us = now_us;
        if (entry->failures >= FAILURES_BEFORE_LOCKOUT)
        {
            const uint32_t doublings = std::min<uint32_t>(entry->failures - FAILURES_BEFORE_LOCKOUT, 10);
            entry->locked_until_us = now_us + std::min(FIRST_LOCKOUT_US << doublings, MAX_LOCKOUT_US);
        }

        if (now_us - s_global_window_start_us > 60LL * 1000000LL)
        {
            s_global_window_start_us = now_us;
            s_global_window_failures = 0;
        }
        if (++s_global_window_failures >= GLOBAL_FAILURES_PER_MINUTE)
        {
            s_global_lockout_us = s_global_lockout_us == 0 ? 60LL * 1000000LL : std::min(s_global_lockout_us * 2, MAX_LOCKOUT_US);
            s_global_locked_until_us = now_us + s_global_lockout_us;
            s_global_last_lockout_us = now_us;
            s_global_window_failures = 0;
            ESP_LOGW(TAG, "Web admin: too many failed logins, all logins blocked for %d s", static_cast<int>(s_global_lockout_us / 1000000LL));
        }
    }

    void record_success(const sockaddr_storage &source)
    {
        uint8_t address[16];
        to_mapped(source, address);
        throttle_entry_t *entry = find_throttle(address, false);
        if (entry != nullptr)
            *entry = throttle_entry_t{};
    }

    bool check_password_hash(const char *password, size_t length)
    {
        if (length == 0 || length > WEB_PASSWORD_MAX_LEN)
            return false;
        uint8_t hash[HASH_SIZE];
        const bool computed = pbkdf2_sha256(password, length, s_password.salt, SALT_SIZE, s_password.iterations, hash);
        const bool equal = web_constant_time_equal(hash, s_password.hash, HASH_SIZE);
        memset(hash, 0, sizeof(hash));
        return computed && equal;
    }

    session_t *find_session(const char *token)
    {
        if (token == nullptr || strlen(token) != WEB_SESSION_TOKEN_HEX_LEN)
            return nullptr;
        uint8_t hash[HASH_SIZE];
        if (!sha256(token, WEB_SESSION_TOKEN_HEX_LEN, hash))
            return nullptr;
        const int64_t now_us = esp_timer_get_time();
        session_t *found = nullptr;
        for (session_t &session : s_sessions)
        {
            if (!session.used)
                continue;
            if (now_us - session.last_used_us > WEB_SESSION_IDLE_TIMEOUT_US || now_us - session.created_us > WEB_SESSION_MAX_AGE_US)
            {
                session = session_t{};
                continue;
            }
            // every used slot is compared, so the time does not tell which one matched
            if (web_constant_time_equal(session.token_hash, hash, HASH_SIZE) && found == nullptr)
                found = &session;
        }
        return found;
    }

    bool load_allow_list()
    {
        nvs_handle_t handle = 0;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
            return false;
        allow_record_t record{};
        size_t length = sizeof(record);
        const bool ok = nvs_get_blob(handle, NVS_KEY_ALLOW, &record, &length) == ESP_OK && length == sizeof(record) &&
                        record.magic == ALLOW_MAGIC && record.count <= WEB_ALLOW_LIST_MAX &&
                        record.crc == crc_of(&record, offsetof(allow_record_t, crc));
        nvs_close(handle);
        if (!ok)
            return false;
        memcpy(s_allow, record.entries, sizeof(s_allow));
        s_allow_count = record.count;
        return true;
    }

    bool entry_matches(const web_allow_entry_t &entry, const uint8_t mapped[16])
    {
        const uint8_t *address = mapped;
        size_t bits = entry.prefix;
        if (entry.family == 4)
        {
            if (!is_v4_mapped(mapped))
                return false;
            address = mapped + 12;
        }
        else if (is_v4_mapped(mapped))
            return false;
        const size_t bytes = bits / 8;
        if (memcmp(address, entry.address, bytes) != 0)
            return false;
        const size_t rest = bits % 8;
        if (rest == 0)
            return true;
        const uint8_t mask = static_cast<uint8_t>(0xFF << (8 - rest));
        return (address[bytes] & mask) == (entry.address[bytes] & mask);
    }
} // namespace

void web_random_hex(char *out, size_t bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    uint8_t buffer[32];
    while (bytes > 0)
    {
        const size_t n = std::min(bytes, sizeof(buffer));
        esp_fill_random(buffer, n);
        for (size_t i = 0; i < n; ++i)
        {
            *out++ = digits[buffer[i] >> 4];
            *out++ = digits[buffer[i] & 0x0F];
        }
        bytes -= n;
    }
    *out = '\0';
    memset(buffer, 0, sizeof(buffer));
}

void web_auth_init()
{
    psa_crypto_init();

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK)
    {
        password_record_t record{};
        size_t length = sizeof(record);
        if (nvs_get_blob(handle, NVS_KEY_PASSWORD, &record, &length) == ESP_OK && length == sizeof(record) &&
            record.magic == PASSWORD_MAGIC && record.iterations >= 1000 && record.iterations <= 1000000 &&
            record.crc == crc_of(&record, offsetof(password_record_t, crc)))
        {
            s_password = record;
            s_password_stored = true;
        }
        nvs_close(handle);
    }

    if (!s_password_stored)
    {
        // Compiled password: hashed with a random salt at every boot, so it is checked exactly like a stored one
        s_password.magic = PASSWORD_MAGIC;
        s_password.iterations = PBKDF2_ITERATIONS;
        esp_fill_random(s_password.salt, SALT_SIZE);
        if (!pbkdf2_sha256(WebAdminPassword, strlen(WebAdminPassword), s_password.salt, SALT_SIZE, PBKDF2_ITERATIONS, s_password.hash))
        {
            ESP_LOGE(TAG, "Web admin: the password hash could not be computed, logins are impossible");
            esp_fill_random(s_password.hash, HASH_SIZE); // nothing matches
        }
    }

    if (load_allow_list() && s_allow_count > 0)
        ESP_LOGI(TAG, "Web admin: source allow-list with %u entries", static_cast<unsigned>(s_allow_count));

    if (web_auth_default_password_active())
        ESP_LOGW(TAG, "Web admin: the default password is in use - change it on the settings page");
}

bool web_auth_default_password_active()
{
    return !s_password_stored && strcmp(WebAdminPassword, FACTORY_PASSWORD) == 0;
}

uint32_t web_auth_login_wait_seconds(const sockaddr_storage &source)
{
    const int64_t now_us = esp_timer_get_time();
    forget_old_failures(now_us);
    int64_t until_us = s_global_locked_until_us;
    uint8_t address[16];
    to_mapped(source, address);
    const throttle_entry_t *entry = find_throttle(address, false);
    if (entry != nullptr)
        until_us = std::max(until_us, entry->locked_until_us);
    return until_us > now_us ? static_cast<uint32_t>((until_us - now_us + 999999LL) / 1000000LL) : 0;
}

bool web_auth_check_password(const sockaddr_storage &source, const char *password, size_t length)
{
    const bool ok = check_password_hash(password, length);
    if (ok)
        record_success(source);
    else
        record_failure(source);
    return ok;
}

bool web_auth_verify_current_password(const sockaddr_storage &source, const char *password, size_t length)
{
    const bool ok = check_password_hash(password, length);
    if (!ok)
        record_failure(source);
    return ok;
}

void web_auth_record_failed_check(const sockaddr_storage &source)
{
    record_failure(source);
}

esp_err_t web_auth_set_password(const char *password, size_t length, const char *keep_session_token)
{
    if (length < WEB_PASSWORD_MIN_LEN || length > WEB_PASSWORD_MAX_LEN)
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(password[i]) < 0x20 || password[i] == 0x7F)
            return ESP_ERR_INVALID_ARG;

    password_record_t record{};
    record.magic = PASSWORD_MAGIC;
    record.iterations = PBKDF2_ITERATIONS;
    esp_fill_random(record.salt, SALT_SIZE);
    if (!pbkdf2_sha256(password, length, record.salt, SALT_SIZE, record.iterations, record.hash))
        return ESP_FAIL;
    record.crc = crc_of(&record, offsetof(password_record_t, crc));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(handle, NVS_KEY_PASSWORD, &record, sizeof(record));
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK)
        return err;

    s_password = record;
    s_password_stored = true;

    // Close all other sessions
    session_t *keep = find_session(keep_session_token);
    for (session_t &session : s_sessions)
        if (&session != keep)
            session = session_t{};
    ESP_LOGI(TAG, "Web admin: password changed");
    return ESP_OK;
}

bool web_auth_create_session(char *token, char *csrf)
{
    const int64_t now_us = esp_timer_get_time();
    session_t *slot = nullptr;
    for (session_t &session : s_sessions)
    {
        if (session.used && (now_us - session.last_used_us > WEB_SESSION_IDLE_TIMEOUT_US || now_us - session.created_us > WEB_SESSION_MAX_AGE_US))
            session = session_t{};
        if (!session.used)
        {
            slot = &session;
            break;
        }
    }
    if (slot == nullptr) // all in use: replace the one used least recently
    {
        slot = &s_sessions[0];
        for (session_t &session : s_sessions)
            if (session.last_used_us < slot->last_used_us)
                slot = &session;
    }

    *slot = session_t{};
    web_random_hex(token, WEB_SESSION_TOKEN_HEX_LEN / 2);
    if (!sha256(token, WEB_SESSION_TOKEN_HEX_LEN, slot->token_hash))
        return false;
    web_random_hex(slot->csrf, WEB_CSRF_TOKEN_HEX_LEN / 2);
    snprintf(csrf, WEB_CSRF_TOKEN_HEX_LEN + 1, "%s", slot->csrf);
    slot->created_us = now_us;
    slot->last_used_us = now_us;
    slot->used = true;
    return true;
}

bool web_auth_session_valid(const char *token, char *csrf)
{
    session_t *session = find_session(token);
    if (session == nullptr)
        return false;
    session->last_used_us = esp_timer_get_time();
    if (csrf != nullptr)
        snprintf(csrf, WEB_CSRF_TOKEN_HEX_LEN + 1, "%s", session->csrf);
    return true;
}

bool web_auth_csrf_valid(const char *token, const char *csrf)
{
    session_t *session = find_session(token);
    if (session == nullptr || csrf == nullptr || strlen(csrf) != WEB_CSRF_TOKEN_HEX_LEN)
        return false;
    return web_constant_time_equal(reinterpret_cast<const uint8_t *>(session->csrf), reinterpret_cast<const uint8_t *>(csrf),
                                   WEB_CSRF_TOKEN_HEX_LEN);
}

void web_auth_end_session(const char *token)
{
    session_t *session = find_session(token);
    if (session != nullptr)
        *session = session_t{};
}

void web_auth_end_all_sessions()
{
    for (session_t &session : s_sessions)
        session = session_t{};
}

size_t web_auth_allow_list(web_allow_entry_t *entries, size_t capacity)
{
    const size_t count = std::min(capacity, s_allow_count);
    memcpy(entries, s_allow, count * sizeof(web_allow_entry_t));
    return count;
}

bool web_auth_source_allowed(const sockaddr_storage &source)
{
    if (s_allow_count == 0)
        return true;
    uint8_t mapped[16];
    to_mapped(source, mapped);
    for (size_t i = 0; i < s_allow_count; ++i)
        if (entry_matches(s_allow[i], mapped))
            return true;
    return false;
}

esp_err_t web_auth_set_allow_list(const web_allow_entry_t *entries, size_t count, const sockaddr_storage &client)
{
    if (count > WEB_ALLOW_LIST_MAX)
        return ESP_ERR_INVALID_ARG;
    if (count > 0)
    {
        uint8_t mapped[16];
        to_mapped(client, mapped);
        bool client_allowed = false;
        for (size_t i = 0; i < count; ++i)
            client_allowed = client_allowed || entry_matches(entries[i], mapped);
        if (!client_allowed)
            return ESP_ERR_INVALID_STATE;
    }

    allow_record_t record{};
    record.magic = ALLOW_MAGIC;
    record.count = count;
    memcpy(record.entries, entries, count * sizeof(web_allow_entry_t));
    record.crc = crc_of(&record, offsetof(allow_record_t, crc));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        err = nvs_set_blob(handle, NVS_KEY_ALLOW, &record, sizeof(record));
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK)
        return err;
    memcpy(s_allow, record.entries, sizeof(s_allow));
    s_allow_count = count;
    ESP_LOGI(TAG, "Web admin: allow-list changed (%u entries)", static_cast<unsigned>(count));
    return ESP_OK;
}

bool web_auth_parse_cidr(const char *text, web_allow_entry_t &entry)
{
    entry = web_allow_entry_t{};
    char address[WEB_IPV6_TEXT_SIZE] = "";
    const char *slash = strchr(text, '/');
    const size_t length = slash == nullptr ? strlen(text) : static_cast<size_t>(slash - text);
    if (length == 0 || length >= sizeof(address))
        return false;
    memcpy(address, text, length);
    address[length] = '\0';

    uint32_t v4 = 0;
    unsigned int max_prefix = 0;
    if (web_parse_ipv4(address, &v4))
    {
        entry.family = 4;
        memcpy(entry.address, &v4, 4);
        max_prefix = 32;
    }
    else if (web_parse_ipv6(address, entry.address))
    {
        entry.family = 6;
        max_prefix = 128;
    }
    else
        return false;

    unsigned int prefix = max_prefix;
    if (slash != nullptr)
    {
        char *end = nullptr;
        const long value = strtol(slash + 1, &end, 10);
        if (end == slash + 1 || *end != '\0' || value < 1 || value > static_cast<long>(max_prefix))
            return false;
        prefix = static_cast<unsigned int>(value);
    }
    entry.prefix = static_cast<uint8_t>(prefix);
    // clear the host bits
    for (unsigned int bit = prefix; bit < max_prefix; ++bit)
        entry.address[bit / 8] &= static_cast<uint8_t>(~(0x80 >> (bit % 8)));
    return true;
}

void web_auth_format_cidr(const web_allow_entry_t &entry, char *out, size_t out_size)
{
    char address[WEB_IPV6_TEXT_SIZE] = "";
    inet_ntop(entry.family == 4 ? AF_INET : AF_INET6, entry.address, address, sizeof(address));
    snprintf(out, out_size, "%s/%u", address, static_cast<unsigned int>(entry.prefix));
}
