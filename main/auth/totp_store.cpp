// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//

#include "auth/totp_store.h"
#include "custom/nvs_blob.h"
#include "sys/record_mark.h"
#include "sys/secret.h"
#include "web/web_settings.h" // app_restart_not_kept()

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

namespace
{
    const char *const TAG = "totp";
    constexpr uint32_t MAGIC = 0x544F5450u; // "TOTP"
    constexpr time_t TIME_SET_AFTER = 1767225600; // 2026-01-01: the clock was set from GNSS

    struct record_t
    {
        uint32_t magic;
        char user[TOTP_USER_SIZE];
        uint8_t secret[TOTP_SECRET_SIZE];
        uint8_t salt[TOTP_SALT_SIZE];
        uint8_t hashes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_HASH];
        int64_t created_s;
        int64_t last_used_s;
        uint64_t last_step;
        uint32_t crc;
    };

    // One NVS chunk: nvs_set_blob() of ESP-IDF 6.1.1 recognises an unchanged record (see custom/nvs_blob.h)
    static_assert(sizeof(record_t) <= NVS_BLOB_ONE_CHUNK_MAX, "a larger record needs nvs_blob_set_if_changed()");

    // The tokens (about 2.7 kB, magic 0: free), taken by totp_store_init
    struct table_t
    {
        record_t token[TOTP_TOKENS_MAX];
    };
    table_t *s_table = nullptr;
    // Slots whose stored token was lost (its mark without a valid record, sys/record_mark.h), found at the start: the
    // mark holds record_mark_text_hash() of the user. That user is refused and gets no new token; the slot is not
    // given to another user. Until NVS is erased over USB.
    bool s_lost[TOTP_TOKENS_MAX] = {};
    uint64_t s_lost_user[TOTP_TOKENS_MAX] = {};
    size_t s_nvs_reserve_entries = 0;

    // NVS entries of 32 bytes for a blob of n bytes: the data, a header per chunk (a blob may span pages) and its index
    constexpr size_t nvs_entries_for(size_t n)
    {
        return (n + 31) / 32 + 4;
    }

    // NVS keeps room for a token and the reserve
    bool nvs_room_for_token()
    {
        nvs_stats_t stats{};
        if (nvs_get_stats(nullptr, &stats) != ESP_OK)
            return true; // unknown: nvs_set_blob tells
        const size_t needed = nvs_entries_for(sizeof(record_t)) + s_nvs_reserve_entries;
        if (stats.available_entries >= needed)
            return true;
        ESP_LOGW(TAG, "TOTP: NVS has %u free entries, a new token needs %u (with the room for the settings)",
                 static_cast<unsigned>(stats.available_entries), static_cast<unsigned>(needed));
        return false;
    }
    SemaphoreHandle_t s_lock = nullptr;
    // A dummy token: a user without one gets the same work as a wrong code
    uint8_t s_dummy_secret[TOTP_SECRET_SIZE];
    uint8_t s_dummy_salt[TOTP_SALT_SIZE];

    class lock_t
    {
    public:
        lock_t() { xSemaphoreTake(s_lock, portMAX_DELAY); }
        ~lock_t() { xSemaphoreGive(s_lock); }
        lock_t(const lock_t &) = delete;
        lock_t &operator=(const lock_t &) = delete;
    };

    uint32_t crc_of(const record_t &r)
    {
        return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&r), offsetof(record_t, crc));
    }

    void key_of(size_t i, char (&key)[4])
    {
        snprintf(key, sizeof(key), "t%u", static_cast<unsigned>(i));
    }

    record_t *find(const char *user)
    {
        for (record_t &r : s_table->token)
            if (r.magic == MAGIC && strcmp(r.user, user) == 0)
                return &r;
        return nullptr;
    }

    bool lost(const char *user)
    {
        const uint64_t hash = record_mark_text_hash(user);
        for (size_t i = 0; i < TOTP_TOKENS_MAX; ++i)
            if (s_lost[i] && s_lost_user[i] == hash)
                return true;
        return false;
    }

    esp_err_t save(size_t i)
    {
        record_t &r = s_table->token[i];
        r.crc = crc_of(r);
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(TOTP_NVS_NAMESPACE, NVS_READWRITE_PURGE, &handle);
        if (err != ESP_OK)
            return err;
        char key[4];
        key_of(i, key);
        err = nvs_set_blob(handle, key, &r, sizeof(r));
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
        // The mark after the record (sys/record_mark.h); written only when it is missing or names another user
        if (err == ESP_OK)
        {
            const esp_err_t mark_err = record_mark_set(TOTP_NVS_NAMESPACE, key, record_mark_text_hash(r.user));
            if (mark_err != ESP_OK)
                ESP_LOGW(TAG, "TOTP: the mark of token %u could not be stored (%s)", static_cast<unsigned>(i), esp_err_to_name(mark_err));
        }
        return err;
    }

    esp_err_t erase(size_t i)
    {
        char key[4];
        key_of(i, key);
        // The mark before the record (sys/record_mark.h): without it the record stays and is loaded at the next start
        esp_err_t err = record_mark_clear(TOTP_NVS_NAMESPACE, key);
        if (err != ESP_OK)
            return err;
        nvs_handle_t handle = 0;
        err = nvs_open(TOTP_NVS_NAMESPACE, NVS_READWRITE_PURGE, &handle);
        if (err != ESP_OK)
            return err;
        err = nvs_erase_key(handle, key);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)
            err = nvs_commit(handle);
        nvs_close(handle);
        return err;
    }

    int64_t now_s()
    {
        const time_t now = time(nullptr);
        return now > TIME_SET_AFTER ? static_cast<int64_t>(now) : 0;
    }
} // namespace

void totp_store_init(size_t nvs_reserve_bytes)
{
    s_nvs_reserve_entries = nvs_reserve_bytes > 0 ? nvs_entries_for(nvs_reserve_bytes) : 0;
    if (s_table == nullptr)
        s_table = static_cast<table_t *>(heap_caps_calloc(1, sizeof(table_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_table == nullptr)
    {
        ESP_LOGE(TAG, "TOTP: no memory for the tokens: logins that need a code are refused");
        return;
    }
    if (s_lock == nullptr)
        s_lock = xSemaphoreCreateMutex();
    esp_fill_random(s_dummy_secret, sizeof(s_dummy_secret));
    esp_fill_random(s_dummy_salt, sizeof(s_dummy_salt));
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(TOTP_NVS_NAMESPACE, NVS_READONLY, &handle);
    // Without memory for the handle (nvs_open() allocates it) nothing about the tokens is known. Taken for "not
    // stored", every stored token would read as lost (its mark): its logins refused for the whole run and a line that
    // says to erase NVS. A restart instead (as for the settings, web/web_settings.cpp)
    if (open_err == ESP_ERR_NO_MEM)
    {
        ESP_LOGE(TAG, "TOTP: the stored tokens could not be read (out of memory): restart");
        app_restart_not_kept("nvs_no_memory");
    }
    const bool opened = open_err == ESP_OK;
    size_t loaded = 0;
    size_t lost_count = 0;
    for (size_t i = 0; i < TOTP_TOKENS_MAX; ++i)
    {
        char key[4];
        key_of(i, key);
        record_t r{};
        const secret_wipe_t wipe_r(&r, sizeof(r)); // the secret of the token
        size_t length = sizeof(r);
        const bool valid = opened && nvs_get_blob(handle, key, &r, &length) == ESP_OK && length == sizeof(r) && r.magic == MAGIC &&
                           r.crc == crc_of(r) && memchr(r.user, '\0', sizeof(r.user)) != nullptr && r.user[0] != '\0';
        if (valid)
        {
            s_table->token[i] = r;
            ++loaded;
        }
        // Every slot, also without the namespace: a mark can be all that is left of a token
        uint64_t user = 0;
        if (record_mark_check(TOTP_NVS_NAMESPACE, key, valid, valid ? record_mark_text_hash(r.user) : 0, &user) == record_mark_state_t::lost)
        {
            s_lost[i] = true;
            s_lost_user[i] = user;
            ++lost_count;
        }
    }
    if (opened)
        nvs_close(handle);
    if (loaded > 0)
        ESP_LOGI(TAG, "TOTP: %u tokens", static_cast<unsigned>(loaded));
    if (lost_count > 0)
        ESP_LOGE(TAG, "TOTP: %u stored tokens were lost (NVS): their users are refused and cannot set up a new token. "
                      "Erase NVS over the USB port to start again with the defaults (README, Lost stored security data)",
                 static_cast<unsigned>(lost_count));
}

bool totp_store_lost(const char *user)
{
    if (s_lock == nullptr || user == nullptr)
        return false;
    lock_t lock;
    return lost(user);
}

size_t totp_store_lost_count()
{
    size_t n = 0;
    for (const bool l : s_lost)
        n += l ? 1 : 0;
    return n;
}

bool totp_store_has(const char *user)
{
    if (s_lock == nullptr || user == nullptr)
        return false;
    lock_t lock;
    return find(user) != nullptr;
}

size_t totp_store_list(totp_token_info_t *out, size_t capacity)
{
    if (s_lock == nullptr)
        return 0;
    lock_t lock;
    size_t n = 0;
    static constexpr uint8_t USED[TOTP_RECOVERY_HASH] = {};
    for (const record_t &r : s_table->token)
    {
        if (r.magic != MAGIC || n == capacity)
            continue;
        totp_token_info_t &o = out[n++];
        snprintf(o.user, sizeof(o.user), "%s", r.user);
        o.created_s = r.created_s;
        o.last_used_s = r.last_used_s;
        o.recovery_left = 0;
        for (const auto &h : r.hashes)
            o.recovery_left += memcmp(h, USED, sizeof(USED)) != 0 ? 1u : 0u;
    }
    return n;
}

size_t totp_store_count()
{
    if (s_lock == nullptr)
        return 0;
    lock_t lock;
    size_t n = 0;
    for (const record_t &r : s_table->token)
        n += r.magic == MAGIC ? 1 : 0;
    return n;
}

void totp_store_new_codes(char codes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_TEXT_SIZE])
{
    for (size_t i = 0; i < TOTP_RECOVERY_CODES; ++i)
    {
        uint8_t random[7];
        const secret_wipe_t wipe_random(random, sizeof(random));
        esp_fill_random(random, sizeof(random));
        totp_recovery_text(random, codes[i]);
    }
}

esp_err_t totp_store_add(const char *user, const uint8_t secret[TOTP_SECRET_SIZE], uint64_t step,
                         const char codes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_TEXT_SIZE])
{
    if (s_lock == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (user == nullptr || user[0] == '\0' || strlen(user) >= TOTP_USER_SIZE)
        return ESP_ERR_INVALID_ARG;
    lock_t lock;
    if (lost(user))
        return ESP_ERR_INVALID_STATE;
    record_t *slot = find(user);
    if (slot == nullptr)
        for (size_t i = 0; i < TOTP_TOKENS_MAX; ++i)
            if (s_table->token[i].magic != MAGIC && !s_lost[i]) // the slot of a lost token stays its own
            {
                slot = &s_table->token[i];
                break;
            }
    if (slot == nullptr)
        return ESP_ERR_NO_MEM;
    if (slot->magic != MAGIC && !nvs_room_for_token()) // a new token (one replaced keeps its size)
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    record_t r{};
    const secret_wipe_t wipe_r(&r, sizeof(r)); // the secret and the hashes of the recovery codes, at every exit
    r.magic = MAGIC;
    snprintf(r.user, sizeof(r.user), "%s", user);
    memcpy(r.secret, secret, TOTP_SECRET_SIZE);
    esp_fill_random(r.salt, sizeof(r.salt));
    for (size_t i = 0; i < TOTP_RECOVERY_CODES; ++i)
    {
        char normalised[11]; // a recovery code as it is checked
        const secret_wipe_t wipe_normalised(normalised, sizeof(normalised));
        if (!totp_recovery_normalise(codes[i], normalised) || !totp_recovery_hash(r.salt, normalised, r.hashes[i]))
            return ESP_FAIL;
    }
    r.created_s = now_s();
    r.last_step = step; // the confirming code is used (the steps of a replaced token belong to its old key)
    record_t previous = *slot; // the token it replaces, for a failed store
    const secret_wipe_t wipe_previous(&previous, sizeof(previous));
    *slot = r;
    const esp_err_t err = save(static_cast<size_t>(slot - s_table->token));
    if (err != ESP_OK)
    {
        *slot = previous;
        ESP_LOGW(TAG, "TOTP: the token of '%s' could not be stored (%s)", user, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TOTP: token of '%s' stored", user);
    return ESP_OK;
}

bool totp_store_remove(const char *user)
{
    if (s_lock == nullptr || user == nullptr)
        return false;
    lock_t lock;
    record_t *r = find(user);
    if (r == nullptr)
        return false;
    const size_t i = static_cast<size_t>(r - s_table->token);
    mbedtls_platform_zeroize(r, sizeof(*r));
    if (erase(i) != ESP_OK)
        ESP_LOGW(TAG, "TOTP: the token %u could not be erased in NVS", static_cast<unsigned>(i));
    ESP_LOGI(TAG, "TOTP: token of '%s' deleted", user);
    return true;
}

bool totp_store_time_ok()
{
    return now_s() != 0;
}

totp_check_t totp_store_check(const char *user, const char *typed)
{
    if (s_lock == nullptr || user == nullptr || typed == nullptr)
        return totp_check_t::wrong;
    lock_t lock;
    record_t *r = find(user);
    const uint8_t *secret = r != nullptr ? r->secret : s_dummy_secret;
    const int64_t now = now_s();
    uint64_t step = 0;
    // a TOTP code (needs the clock) or a recovery code; the same work with the dummy token
    const bool code = now != 0 && totp_verify(secret, TOTP_SECRET_SIZE, now, typed, r != nullptr ? r->last_step : 0, step);
    static uint8_t dummy_hashes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_HASH];
    const int recovery = code ? -1 : totp_recovery_find(r != nullptr ? r->salt : s_dummy_salt, r != nullptr ? r->hashes : dummy_hashes, typed);
    if (r == nullptr)
        return totp_check_t::no_token;
    if (!code && recovery < 0)
        return totp_check_t::wrong;
    if (code)
        r->last_step = step;
    else
        memset(r->hashes[recovery], 0, TOTP_RECOVERY_HASH);
    r->last_used_s = now;
    if (save(static_cast<size_t>(r - s_table->token)) != ESP_OK)
        ESP_LOGW(TAG, "TOTP: the use of the token of '%s' could not be stored", user);
    if (!code)
        ESP_LOGW(TAG, "TOTP: '%s' used a recovery code", user);
    return code ? totp_check_t::code : totp_check_t::recovery;
}
