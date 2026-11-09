// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//

#include "auth/totp_store.h"

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

    // The tokens (about 2.7 kB, magic 0: free), taken by totp_store_init
    struct table_t
    {
        record_t token[TOTP_TOKENS_MAX];
    };
    table_t *s_table = nullptr;
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

    esp_err_t save(size_t i)
    {
        record_t &r = s_table->token[i];
        r.crc = crc_of(r);
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(TOTP_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK)
            return err;
        char key[4];
        key_of(i, key);
        err = nvs_set_blob(handle, key, &r, sizeof(r));
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
        return err;
    }

    esp_err_t erase(size_t i)
    {
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(TOTP_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK)
            return err;
        char key[4];
        key_of(i, key);
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
    if (nvs_open(TOTP_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;
    size_t loaded = 0;
    for (size_t i = 0; i < TOTP_TOKENS_MAX; ++i)
    {
        char key[4];
        key_of(i, key);
        record_t r{};
        size_t length = sizeof(r);
        if (nvs_get_blob(handle, key, &r, &length) == ESP_OK && length == sizeof(r) && r.magic == MAGIC && r.crc == crc_of(r) &&
            memchr(r.user, '\0', sizeof(r.user)) != nullptr && r.user[0] != '\0')
        {
            s_table->token[i] = r;
            ++loaded;
        }
        memset(&r, 0, sizeof(r));
    }
    nvs_close(handle);
    if (loaded > 0)
        ESP_LOGI(TAG, "TOTP: %u tokens", static_cast<unsigned>(loaded));
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

esp_err_t totp_store_add(const char *user, const uint8_t secret[TOTP_SECRET_SIZE], char codes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_TEXT_SIZE])
{
    if (s_lock == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (user == nullptr || user[0] == '\0' || strlen(user) >= TOTP_USER_SIZE)
        return ESP_ERR_INVALID_ARG;
    lock_t lock;
    record_t *slot = find(user);
    if (slot == nullptr)
        for (record_t &r : s_table->token)
            if (r.magic != MAGIC)
            {
                slot = &r;
                break;
            }
    if (slot == nullptr)
        return ESP_ERR_NO_MEM;
    if (slot->magic != MAGIC && !nvs_room_for_token()) // a new token (one replaced keeps its size)
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    record_t r{};
    r.magic = MAGIC;
    snprintf(r.user, sizeof(r.user), "%s", user);
    memcpy(r.secret, secret, TOTP_SECRET_SIZE);
    esp_fill_random(r.salt, sizeof(r.salt));
    for (size_t i = 0; i < TOTP_RECOVERY_CODES; ++i)
    {
        uint8_t random[7];
        char normalised[11];
        esp_fill_random(random, sizeof(random));
        totp_recovery_text(random, codes[i]);
        if (!totp_recovery_normalise(codes[i], normalised) || !totp_recovery_hash(r.salt, normalised, r.hashes[i]))
            return ESP_FAIL;
        memset(random, 0, sizeof(random));
    }
    r.created_s = now_s();
    const record_t previous = *slot;
    *slot = r;
    memset(&r, 0, sizeof(r));
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
    memset(r, 0, sizeof(*r));
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
