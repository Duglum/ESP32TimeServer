// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "web_tls.h"
#include "web_tls_pair.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

#include "web_settings.h"

extern "C"
{
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"
}

namespace
{
    const char *TAG = "web_tls";

    constexpr char NVS_NAMESPACE[] = "webtls";
    constexpr char NVS_KEY_KEY[] = "key";      // private key of the self-signed certificate, DER
    constexpr char NVS_KEY_CERT[] = "crt";     // self-signed certificate, DER
    constexpr char NVS_KEY_NAME[] = "name";    // FQDN the self-signed certificate was issued for
    constexpr char NVS_KEY_MODE[] = "mode";    // 1: the own certificate is in use
    // The own chain and key in three layouts (web_tls_pair.h): "opair2" with the marker in "okey" (this firmware),
    // "opair" (478c2ff to d77f972), "ocrt" and "okey" (before 478c2ff: each PEM with its NUL, written one after the
    // other)
    constexpr char NVS_KEY_OWN_PAIR2[] = "opair2";
    constexpr char NVS_KEY_OWN_PAIR[] = "opair";
    constexpr char NVS_KEY_OWN_CERT[] = "ocrt";
    constexpr char NVS_KEY_OWN_KEY[] = "okey";
    constexpr size_t OWN_PAIR_MAX = web_tls_pair_size(WEB_TLS_CERT_PEM_MAX, WEB_TLS_KEY_PEM_MAX);
    constexpr size_t MAX_DER = 1024;

    // Fixed validity: the time may not be known yet when the certificate is created
    constexpr char NOT_BEFORE[] = "20260101000000";
    constexpr char NOT_AFTER[] = "20991231235959";

    // The identities. Used by the web server task only (start and its handlers), so no lock.
    uint8_t *s_self_cert = nullptr;
    size_t s_self_cert_length = 0;
    uint8_t *s_self_key = nullptr;
    size_t s_self_key_length = 0;
    char s_self_fingerprint[96] = "";
    char *s_own_cert = nullptr; // PEM
    char *s_own_key = nullptr;  // PEM
    char s_own_fingerprint[96] = "";
    // What the stored own certificate contains, from its last check (valid while s_own_cert is set): /api/state
    // reads it every 10 s, and a check signs with the key (RSA 4096: a noticeable time in the server task)
    web_tls_info_t s_own_info{};
    bool s_own_mode = false;
    // NVS holds another pair than s_own_cert/s_own_key: a store failed after its pair was stored (until the restart;
    // the next store of any pair writes it)
    bool s_own_differs = false;

    void *alloc_prefer_psram(size_t size)
    {
        return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
    }

    char *copy_text(const char *text)
    {
        const size_t length = strlen(text) + 1;
        char *copy = static_cast<char *>(alloc_prefer_psram(length));
        if (copy != nullptr)
            memcpy(copy, text, length);
        return copy;
    }

    void free_secret(char *&text)
    {
        if (text != nullptr)
        {
            mbedtls_platform_zeroize(text, strlen(text));
            free(text);
        }
        text = nullptr;
    }

    // length: the bytes of data. On an error of the second read NVS may set the length to the stored size, which can
    // be more than was allocated (ESP-IDF 6.1.1 nvs_api.cpp 616-618): the size of the buffer is kept apart.
    bool read_blob(nvs_handle_t handle, const char *key, uint8_t *&data, size_t &length, size_t max)
    {
        data = nullptr;
        length = 0;
        size_t size = 0;
        if (nvs_get_blob(handle, key, nullptr, &size) != ESP_OK || size == 0 || size > max)
            return false;
        uint8_t *buffer = static_cast<uint8_t *>(alloc_prefer_psram(size));
        if (buffer == nullptr)
            return false;
        size_t read = size;
        if (nvs_get_blob(handle, key, buffer, &read) != ESP_OK || read != size)
        {
            mbedtls_platform_zeroize(buffer, size);
            free(buffer);
            return false;
        }
        data = buffer;
        length = size;
        return true;
    }

    void free_data_secret(uint8_t *&data, size_t length)
    {
        if (data != nullptr)
        {
            mbedtls_platform_zeroize(data, length);
            free(data);
        }
        data = nullptr;
    }

    // The own items in NVS as the rule needs them (web_tls_pair.h), with their texts
    struct own_nvs_t
    {
        web_tls_own_items_t items;
        char *ocrt = nullptr;                 // the text of "ocrt" (NUL at the end), else nullptr
        char *okey = nullptr;                 // the text of "okey" when it is a key, else nullptr
        size_t ocrt_size = 0, okey_size = 0;  // stored bytes (also when there is no text)
        uint8_t *pair[2] = {};                // the blobs of "opair" and "opair2"
        size_t pair_size[2] = {};             // stored bytes (also when the blob was not read)
        const char *pair_cert[2] = {};        // into pair: nullptr when the blob does not split
        const char *pair_key[2] = {};
    };

    constexpr const char *OWN_PAIR_KEYS[2] = {NVS_KEY_OWN_PAIR, NVS_KEY_OWN_PAIR2};
    constexpr uint8_t OWN_PAIR_IN[2] = {WEB_TLS_OWN_IN_OPAIR, WEB_TLS_OWN_IN_OPAIR2};

    void free_own(own_nvs_t &o)
    {
        free_secret(o.ocrt);
        free_secret(o.okey);
        for (int i = 0; i < 2; ++i)
            free_data_secret(o.pair[i], o.pair_size[i]);
        o = own_nvs_t{};
    }

    // A text item of at most max characters: present tells whether it exists; text is nullptr when it is no text
    // (no NUL at the end, or longer). False on an NVS error other than a missing item, or out of memory.
    bool read_own_text(nvs_handle_t handle, const char *key, size_t max, bool &present, size_t &size, char *&text)
    {
        present = false;
        text = nullptr;
        const esp_err_t err = nvs_get_blob(handle, key, nullptr, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            return true;
        if (err != ESP_OK)
            return false;
        present = true;
        if (size == 0 || size > max + 1)
            return true;
        uint8_t *data = nullptr;
        size_t length = 0;
        if (!read_blob(handle, key, data, length, max + 1))
            return false;
        if (data[length - 1] != '\0')
            free_data_secret(data, length);
        else
            text = reinterpret_cast<char *>(data);
        return true;
    }

    // Reads the own items. False on an NVS error other than a missing item, or out of memory: then the items are not
    // known for sure and nothing is erased or written.
    bool read_own(nvs_handle_t handle, own_nvs_t &o)
    {
        o = own_nvs_t{};
        bool ok = true;
        for (int i = 0; i < 2; ++i)
        {
            const esp_err_t err = nvs_get_blob(handle, OWN_PAIR_KEYS[i], nullptr, &o.pair_size[i]);
            if (err == ESP_ERR_NVS_NOT_FOUND)
                continue;
            if (err != ESP_OK)
            {
                ok = false;
                continue;
            }
            (i == 0 ? o.items.opair : o.items.opair2) = true;
            if (o.pair_size[i] == 0 || o.pair_size[i] > OWN_PAIR_MAX)
                continue; // cannot be used
            size_t length = 0;
            if (!read_blob(handle, OWN_PAIR_KEYS[i], o.pair[i], length, OWN_PAIR_MAX))
            {
                ok = false;
                continue;
            }
            if (!web_tls_pair_split(o.pair[i], o.pair_size[i], o.pair_cert[i], o.pair_key[i]))
                o.pair_cert[i] = o.pair_key[i] = nullptr;
        }
        ok = read_own_text(handle, NVS_KEY_OWN_CERT, WEB_TLS_CERT_PEM_MAX, o.items.ocrt, o.ocrt_size, o.ocrt) && ok;
        // okey: the marker or a key
        esp_err_t err = nvs_get_blob(handle, NVS_KEY_OWN_KEY, nullptr, &o.okey_size);
        if (err == ESP_OK)
        {
            o.items.okey = web_tls_okey_t::key;
            if (o.okey_size == WEB_TLS_OWN_MARKER_SIZE)
            {
                char marker[WEB_TLS_OWN_MARKER_SIZE];
                size_t length = sizeof(marker);
                err = nvs_get_blob(handle, NVS_KEY_OWN_KEY, marker, &length);
                if (err != ESP_OK)
                    ok = false;
                else if (memcmp(marker, WEB_TLS_OWN_MARKER, WEB_TLS_OWN_MARKER_SIZE) == 0)
                    o.items.okey = web_tls_okey_t::marker;
            }
            bool present = false;
            if (o.items.okey == web_tls_okey_t::key)
                ok = read_own_text(handle, NVS_KEY_OWN_KEY, WEB_TLS_KEY_PEM_MAX, present, o.okey_size, o.okey) && ok;
        }
        else if (err != ESP_ERR_NVS_NOT_FOUND)
            ok = false;
        uint8_t mode = 0;
        err = nvs_get_u8(handle, NVS_KEY_MODE, &mode);
        if (err == ESP_OK)
            o.items.mode = true;
        else if (err != ESP_ERR_NVS_NOT_FOUND)
            ok = false;
        // Which halves are the halves of a whole item
        for (int i = 0; i < 2; ++i)
        {
            if (o.ocrt != nullptr && o.pair_cert[i] != nullptr && strcmp(o.ocrt, o.pair_cert[i]) == 0)
                o.items.ocrt_in |= OWN_PAIR_IN[i];
            if (o.okey != nullptr && o.pair_key[i] != nullptr && strcmp(o.okey, o.pair_key[i]) == 0)
                o.items.okey_in |= OWN_PAIR_IN[i];
        }
        return ok;
    }

    const char *step_name(web_tls_own_step_t step)
    {
        switch (step)
        {
        case web_tls_own_step_t::erase_ocrt: return "erase ocrt";
        case web_tls_own_step_t::erase_okey: return "erase okey";
        case web_tls_own_step_t::erase_opair: return "erase opair";
        case web_tls_own_step_t::erase_opair2: return "erase opair2";
        case web_tls_own_step_t::write_marker: return "write the marker into okey";
        case web_tls_own_step_t::copy_opair: return "copy opair into opair2";
        case web_tls_own_step_t::write_new: return "write opair2";
        }
        return "?";
    }

    // Runs the steps of a plan and stops at the first error. Returns the index of the step that failed (plan.count:
    // none); err its error.
    uint8_t run_steps(nvs_handle_t handle, const web_tls_own_plan_t &plan, const own_nvs_t &o, const uint8_t *new_pair, size_t new_size, esp_err_t &err)
    {
        err = ESP_OK;
        for (uint8_t i = 0; i < plan.count; ++i)
        {
            switch (plan.steps[i])
            {
            case web_tls_own_step_t::erase_ocrt:
                err = nvs_erase_key(handle, NVS_KEY_OWN_CERT);
                break;
            case web_tls_own_step_t::erase_okey:
                err = nvs_erase_key(handle, NVS_KEY_OWN_KEY);
                break;
            case web_tls_own_step_t::erase_opair:
                err = nvs_erase_key(handle, NVS_KEY_OWN_PAIR);
                break;
            case web_tls_own_step_t::erase_opair2:
                err = nvs_erase_key(handle, NVS_KEY_OWN_PAIR2);
                break;
            case web_tls_own_step_t::write_marker:
                err = nvs_set_blob(handle, NVS_KEY_OWN_KEY, WEB_TLS_OWN_MARKER, WEB_TLS_OWN_MARKER_SIZE);
                break;
            case web_tls_own_step_t::copy_opair:
                err = o.pair_cert[0] != nullptr ? nvs_set_blob(handle, NVS_KEY_OWN_PAIR2, o.pair[0], o.pair_size[0]) : ESP_ERR_INVALID_STATE;
                break;
            case web_tls_own_step_t::write_new:
                err = new_pair != nullptr ? nvs_set_blob(handle, NVS_KEY_OWN_PAIR2, new_pair, new_size) : ESP_ERR_INVALID_ARG;
                break;
            }
            if (err == ESP_ERR_NVS_NOT_FOUND)
                err = ESP_OK; // an erase of an item that is gone already
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "Own certificate: NVS step \"%s\" failed (%s)", step_name(plan.steps[i]), esp_err_to_name(err));
                return i;
            }
        }
        return plan.count;
    }

    const char *source_name(web_tls_own_source_t source)
    {
        switch (source)
        {
        case web_tls_own_source_t::former: return "ocrt/okey (firmware before 478c2ff)";
        case web_tls_own_source_t::opair: return "opair (firmware 478c2ff to d77f972)";
        case web_tls_own_source_t::opair2: return "opair2";
        default: return "none";
        }
    }

    // The stored own chain and key into s_own_cert and s_own_key (not checked here), the pair the rule takes
    // (web_tls_pair.h); then the steps of the start: items that no longer count are erased, a pair in "opair" is moved
    // into "opair2"
    void load_own(nvs_handle_t handle)
    {
        own_nvs_t o;
        if (!read_own(handle, o))
        {
            ESP_LOGW(TAG, "Own certificate not used: its NVS items could not be read");
            free_own(o);
            return;
        }
        const web_tls_own_plan_t plan = web_tls_own_start(o.items);
        bool usable = true;
        switch (plan.source)
        {
        case web_tls_own_source_t::former:
            if (o.ocrt != nullptr)
            {
                s_own_cert = copy_text(o.ocrt);
                s_own_key = o.okey != nullptr ? copy_text(o.okey) : nullptr;
            }
            break;
        case web_tls_own_source_t::opair:
        case web_tls_own_source_t::opair2:
        {
            const int i = plan.source == web_tls_own_source_t::opair ? 0 : 1;
            usable = o.pair_cert[i] != nullptr;
            if (usable)
            {
                s_own_cert = copy_text(o.pair_cert[i]);
                s_own_key = copy_text(o.pair_key[i]);
            }
            else
                ESP_LOGW(TAG, "Own certificate not used: the stored certificate and key cannot be read (%s)", OWN_PAIR_KEYS[i]);
            break;
        }
        default:
            break;
        }
        if ((s_own_cert == nullptr) != (s_own_key == nullptr) && plan.source != web_tls_own_source_t::former)
        {
            ESP_LOGE(TAG, "Own certificate not used: out of memory");
            free_secret(s_own_cert);
            free_secret(s_own_key);
        }
        // The steps only when the pair that counts could be read: an item that cannot be read stays as it is
        if (usable && plan.count > 0)
        {
            esp_err_t err = ESP_OK;
            const uint8_t failed = run_steps(handle, plan, o, nullptr, 0, err);
            nvs_commit(handle);
            if (failed < plan.count)
                ESP_LOGW(TAG, "Own certificate: %u of %u NVS steps done (%s), the next start goes on", static_cast<unsigned>(failed), static_cast<unsigned>(plan.count), esp_err_to_name(err));
            else
                ESP_LOGI(TAG, "Own certificate from %s: %u NVS steps done", source_name(plan.source), static_cast<unsigned>(plan.count));
        }
        free_own(o);
    }

    // Before anything is written: room for the new pair (and the other items the store writes) next to the stored
    // data, and afterwards still room for writing the settings again (Save writes the new set before it erases the old
    // one), as for a new TOTP token
    bool room_for_pair(const own_nvs_t &o, const web_tls_own_plan_t &plan, size_t size, char *err, size_t err_size)
    {
        nvs_stats_t stats{};
        if (nvs_get_stats(nullptr, &stats) != ESP_OK)
            return true; // unknown: nvs_set_blob tells, and it stores all or nothing
        // What the store erases or replaces once the new pair is stored
        size_t old_entries = 0;
        for (int i = 0; i < 2; ++i)
            if (i == 0 ? o.items.opair : o.items.opair2)
                old_entries += web_tls_nvs_entries_min(o.pair_size[i]);
        if (o.items.ocrt)
            old_entries += web_tls_nvs_entries_min(o.ocrt_size);
        if (o.items.okey == web_tls_okey_t::key)
            old_entries += web_tls_nvs_entries_min(o.okey_size);
        size_t extra = 0;
        for (uint8_t i = 0; i < plan.count; ++i)
            if (plan.steps[i] == web_tls_own_step_t::write_marker)
                extra += web_tls_nvs_entries_max(WEB_TLS_OWN_MARKER_SIZE);
            else if (plan.steps[i] == web_tls_own_step_t::copy_opair)
                extra += web_tls_nvs_entries_max(o.pair_size[0]);
        size_t needed = 0;
        if (web_tls_nvs_room(stats.available_entries, size, extra, old_entries, web_tls_nvs_entries_max(web_settings_stored_size()), needed))
            return true;
        snprintf(err, err_size, "The certificate could not be stored: NVS has %u free entries, %u are needed (the certificate and key, and room for writing the settings again)",
                 static_cast<unsigned>(stats.available_entries), static_cast<unsigned>(needed));
        ESP_LOGW(TAG, "%s", err);
        return false;
    }

    // Stores chain and key (web_tls_own_store). room: false when the room check refused it (nothing written, err
    // filled). stored: the new pair is the one NVS holds now (also when a later step failed).
    esp_err_t store_pair(nvs_handle_t handle, const char *cert, const char *key, bool &stored, bool &room, char *err, size_t err_size)
    {
        stored = false;
        room = true;
        own_nvs_t o;
        if (!read_own(handle, o))
        {
            free_own(o);
            return ESP_FAIL;
        }
        o.items.new_in = static_cast<uint8_t>((o.ocrt != nullptr && strcmp(o.ocrt, cert) == 0 ? 1 : 0) | (o.okey != nullptr && strcmp(o.okey, key) == 0 ? 2 : 0));
        const web_tls_own_plan_t plan = web_tls_own_store(o.items);
        const size_t size = web_tls_pair_size(strlen(cert), strlen(key));
        if (!room_for_pair(o, plan, size, err, err_size))
        {
            room = false;
            free_own(o);
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        uint8_t *data = static_cast<uint8_t *>(alloc_prefer_psram(size));
        esp_err_t result = data == nullptr ? ESP_ERR_NO_MEM : web_tls_pair_pack(cert, key, data, size) == size ? ESP_OK : ESP_ERR_INVALID_ARG;
        if (result == ESP_OK)
        {
            const uint8_t failed = run_steps(handle, plan, o, data, size, result);
            stored = failed > plan.commit;
        }
        free_data_secret(data, size);
        free_own(o);
        return result;
    }

    bool fingerprint(const uint8_t *der, size_t length, char out[96])
    {
        uint8_t hash[32];
        size_t hash_length = 0;
        if (psa_hash_compute(PSA_ALG_SHA_256, der, length, hash, sizeof(hash), &hash_length) != PSA_SUCCESS)
            return false;
        char *p = out;
        for (size_t i = 0; i < sizeof(hash); ++i)
            p += snprintf(p, 4, i == 0 ? "%02X" : ":%02X", hash[i]);
        return true;
    }

    // Days since 1970-01-01 of a civil date (proleptic Gregorian)
    int64_t days_from_civil(int year, int month, int day)
    {
        year -= month <= 2 ? 1 : 0;
        const int era = (year >= 0 ? year : year - 399) / 400;
        const int yoe = year - era * 400;
        const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return static_cast<int64_t>(era) * 146097 + doe - 719468;
    }

    int64_t unix_time(const mbedtls_x509_time &t)
    {
        return days_from_civil(t.year, t.mon, t.day) * 86400 + t.hour * 3600 + t.min * 60 + t.sec;
    }

    // The private key belongs to the public key of the certificate: a signature over a random hash made with it is
    // verified with the certificate's key. mbedtls_pk_check_pair() alone is not used: it compares the exported public
    // keys byte for byte and refused matching RSA 2048 pairs on the device (board test 1.13); its result is logged.
    bool key_belongs(mbedtls_pk_context &pub, mbedtls_pk_context &prv)
    {
        uint8_t hash[32];
        esp_fill_random(hash, sizeof(hash));
        uint8_t *signature = static_cast<uint8_t *>(alloc_prefer_psram(MBEDTLS_PK_SIGNATURE_MAX_SIZE));
        if (signature == nullptr)
            return false;
        size_t length = 0;
        int ret = mbedtls_pk_sign(&prv, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, MBEDTLS_PK_SIGNATURE_MAX_SIZE, &length);
        if (ret == 0)
            ret = mbedtls_pk_verify(&pub, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, length);
        free(signature);
        const int pair = mbedtls_pk_check_pair(&pub, &prv);
        if (ret == 0 && pair != 0)
            ESP_LOGI(TAG, "mbedtls_pk_check_pair refused the pair (-0x%04x), the signature check accepts it", static_cast<unsigned>(-pair));
        else if (ret != 0)
            ESP_LOGI(TAG, "Signature check of the key failed (-0x%04x)", static_cast<unsigned>(ret < 0 ? -ret : ret));
        return ret == 0;
    }

    // The TLS 1.3 Certificate message the web server sends with this chain, as ssl_tls13_write_certificate_body() of
    // mbedTLS writes it: handshake header 4, certificate_request_context 1 (empty for a server), length of the list 3,
    // and per certificate a length of 3, its DER and empty extensions of 2. mbedTLS writes the whole message into one
    // record of at most MBEDTLS_SSL_OUT_CONTENT_LEN bytes (mbedtls_ssl_start_handshake_msg(); the dynamic buffers of
    // ESP-IDF do not change this) and does not split it: with a larger one every handshake fails (-0x008A, buffer too
    // small) and the page cannot be opened. esp-tls parses the same PEM with the same function, so the chain is the same.
    size_t certificate_message_size(const mbedtls_x509_crt &chain)
    {
        size_t size = 4 + 1 + 3;
        for (const mbedtls_x509_crt *c = &chain; c != nullptr && c->raw.len > 0; c = c->next)
            size += 3 + c->raw.len + 2;
        return size;
    }

    // Checks a chain and key and fills info; the parsed chain and key are freed again
    bool check(const char *cert_pem, const char *key_pem, web_tls_info_t &info, char *err, size_t err_size)
    {
        info = web_tls_info_t{};
        if (cert_pem == nullptr || cert_pem[0] == '\0')
        {
            snprintf(err, err_size, "Certificate: paste the certificate chain (PEM)");
            return false;
        }
        if (strnlen(cert_pem, WEB_TLS_CERT_PEM_MAX + 1) > WEB_TLS_CERT_PEM_MAX || !web_pem_certificates_valid(cert_pem))
        {
            snprintf(err, err_size, "Certificate: PEM with one or more BEGIN CERTIFICATE / END CERTIFICATE blocks expected, at most %u characters",
                     static_cast<unsigned>(WEB_TLS_CERT_PEM_MAX));
            return false;
        }
        if (key_pem == nullptr || key_pem[0] == '\0')
        {
            snprintf(err, err_size, "No private key stored: paste the private key (PEM)");
            return false;
        }
        // "ENCRYPTED PRIVATE KEY" (PKCS#8) or "Proc-Type: 4,ENCRYPTED" (traditional): its own message
        if (strstr(key_pem, "ENCRYPTED") != nullptr)
        {
            snprintf(err, err_size, "Private key: encrypted; export it without a password");
            return false;
        }
        if (strnlen(key_pem, WEB_TLS_KEY_PEM_MAX + 1) > WEB_TLS_KEY_PEM_MAX || !web_pem_private_key_valid(key_pem))
        {
            snprintf(err, err_size, "Private key: one PEM block BEGIN PRIVATE KEY, EC PRIVATE KEY or RSA PRIVATE KEY expected (not encrypted), at most %u characters",
                     static_cast<unsigned>(WEB_TLS_KEY_PEM_MAX));
            return false;
        }
        mbedtls_x509_crt chain;
        mbedtls_x509_crt_init(&chain);
        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        bool ok = false;
        int ret = mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char *>(cert_pem), strlen(cert_pem) + 1);
        size_t message_size = 0;
        if (ret != 0)
            snprintf(err, err_size, "Certificate: not a valid X.509 certificate chain (mbedTLS -0x%04x)", static_cast<unsigned>(ret < 0 ? -ret : ret));
        else if ((message_size = certificate_message_size(chain)) > MBEDTLS_SSL_OUT_CONTENT_LEN)
            snprintf(err, err_size, "Certificate: the chain needs %u bytes in the TLS handshake, at most %u fit; leave out the root certificate",
                     static_cast<unsigned>(message_size), static_cast<unsigned>(MBEDTLS_SSL_OUT_CONTENT_LEN));
        else if ((ret = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char *>(key_pem), strlen(key_pem) + 1, nullptr, 0)) != 0)
        {
            if (ret == MBEDTLS_ERR_PK_PASSWORD_REQUIRED)
                snprintf(err, err_size, "Private key: encrypted; export it without a password");
            else
                snprintf(err, err_size, "Private key: could not be read (mbedTLS -0x%04x)", static_cast<unsigned>(-ret));
        }
        else
        {
            const psa_key_type_t type = mbedtls_pk_get_key_type(&key);
            const size_t bits = mbedtls_pk_get_bitlen(&key);
            if (PSA_KEY_TYPE_IS_ECC(type) && PSA_KEY_TYPE_ECC_GET_FAMILY(type) == PSA_ECC_FAMILY_SECP_R1 && (bits == 256 || bits == 384))
                snprintf(info.key_type, sizeof(info.key_type), "ECDSA P-%u", static_cast<unsigned>(bits));
            else if (PSA_KEY_TYPE_IS_RSA(type) && bits >= 2048 && bits <= 4096)
                snprintf(info.key_type, sizeof(info.key_type), "RSA %u", static_cast<unsigned>(bits));
            if (info.key_type[0] == '\0')
                snprintf(err, err_size, "Private key: only ECDSA P-256, P-384 or RSA with 2048 to 4096 bits");
            else if (!key_belongs(chain.pk, key))
                snprintf(err, err_size, "The private key does not belong to the first certificate of the chain");
            else
                ok = fingerprint(chain.raw.p, chain.raw.len, info.fingerprint);
        }
        if (ok)
        {
            mbedtls_x509_dn_gets(info.subject, sizeof(info.subject), &chain.subject);
            mbedtls_x509_dn_gets(info.issuer, sizeof(info.issuer), &chain.issuer);
            info.not_before = unix_time(chain.valid_from);
            info.not_after = unix_time(chain.valid_to);
            for (const mbedtls_x509_crt *c = &chain; c != nullptr && c->raw.len > 0; c = c->next)
                ++info.chain_length;
            for (const mbedtls_x509_sequence *cur = &chain.subject_alt_names; cur != nullptr && info.san_count < WEB_TLS_SAN_MAX; cur = cur->next)
            {
                if (cur->buf.len == 0)
                    continue;
                mbedtls_x509_subject_alternative_name san{};
                if (mbedtls_x509_parse_subject_alt_name(&cur->buf, &san) != 0)
                    continue; // a type mbedTLS does not parse
                char *out = info.san[info.san_count];
                const size_t length = san.san.unstructured_name.len;
                if (san.type == MBEDTLS_X509_SAN_DNS_NAME && length < sizeof(info.san[0]))
                {
                    memcpy(out, san.san.unstructured_name.p, length);
                    out[length] = '\0';
                    ++info.san_count;
                }
                else if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS && (length == 4 || length == 16))
                {
                    if (inet_ntop(length == 4 ? AF_INET : AF_INET6, san.san.unstructured_name.p, out, sizeof(info.san[0])) != nullptr)
                        ++info.san_count;
                }
                mbedtls_x509_free_subject_alt_name(&san);
            }
        }
        mbedtls_pk_free(&key);
        mbedtls_x509_crt_free(&chain);
        return ok;
    }

    void fill(web_tls_identity_t &identity)
    {
        identity = web_tls_identity_t{};
        identity.own = s_own_mode && s_own_cert != nullptr && s_own_key != nullptr;
        if (identity.own)
        {
            identity.certificate = reinterpret_cast<const uint8_t *>(s_own_cert);
            identity.certificate_length = strlen(s_own_cert) + 1;
            identity.key = reinterpret_cast<const uint8_t *>(s_own_key);
            identity.key_length = strlen(s_own_key) + 1;
            snprintf(identity.fingerprint, sizeof(identity.fingerprint), "%s", s_own_fingerprint);
        }
        else
        {
            identity.certificate = s_self_cert;
            identity.certificate_length = s_self_cert_length;
            identity.key = s_self_key;
            identity.key_length = s_self_key_length;
            snprintf(identity.fingerprint, sizeof(identity.fingerprint), "%s", s_self_fingerprint);
        }
    }

    // Generates a new P-256 key pair with the PSA RNG (hardware entropy) and returns it as a PK context
    bool generate_key(mbedtls_pk_context &pk)
    {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attributes, 256);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        psa_key_id_t key_id = 0;
        if (psa_generate_key(&attributes, &key_id) != PSA_SUCCESS)
            return false;
        const bool ok = mbedtls_pk_copy_from_psa(key_id, &pk) == 0;
        psa_destroy_key(key_id);
        return ok;
    }

    // Issues the self-signed certificate. Returns the DER length (0 on error); the DER is at the end of buf.
    size_t issue_certificate(mbedtls_pk_context &pk, const char *fqdn, uint8_t *buf, size_t size, uint8_t *&der)
    {
        mbedtls_x509write_cert crt;
        mbedtls_x509write_crt_init(&crt);
        char subject[300];
        snprintf(subject, sizeof(subject), "CN=%s,O=ESP32 Time Server", fqdn);

        uint8_t serial[16];
        esp_fill_random(serial, sizeof(serial));
        serial[0] &= 0x7F; // positive
        serial[0] |= 0x40; // no leading zero byte

        mbedtls_x509_san_list san{};
        san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san.node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(const_cast<char *>(fqdn));
        san.node.san.unstructured_name.len = strlen(fqdn);
        san.next = nullptr;

        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        int ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_validity(&crt, NOT_BEFORE, NOT_AFTER);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san);
        if (ret == 0)
            ret = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
        if (ret == 0)
            ret = mbedtls_x509write_crt_der(&crt, buf, size);
        mbedtls_x509write_crt_free(&crt);
        if (ret <= 0)
        {
            ESP_LOGE(TAG, "Certificate could not be created (-0x%04x)", static_cast<unsigned int>(-ret));
            return 0;
        }
        der = buf + size - ret;
        return static_cast<size_t>(ret);
    }

    // The self-signed identity: loaded, or created when missing or issued for another name
    bool load_or_create_self(nvs_handle_t handle, const char *fqdn)
    {
        char stored_name[256] = "";
        size_t name_length = sizeof(stored_name);
        const bool have_name = nvs_get_str(handle, NVS_KEY_NAME, stored_name, &name_length) == ESP_OK;
        const bool have_key = read_blob(handle, NVS_KEY_KEY, s_self_key, s_self_key_length, MAX_DER);
        const bool have_cert = read_blob(handle, NVS_KEY_CERT, s_self_cert, s_self_cert_length, MAX_DER);

        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        const bool key_ok = have_key && mbedtls_pk_parse_key(&pk, s_self_key, s_self_key_length, nullptr, 0) == 0;
        const bool ok = key_ok && have_cert && have_name && strcmp(stored_name, fqdn) == 0;

        if (!ok)
        {
            bool new_key = false;
            if (!key_ok)
            {
                mbedtls_pk_free(&pk);
                mbedtls_pk_init(&pk);
                ESP_LOGI(TAG, "Creating the TLS key of the web admin (ECDSA P-256)");
                if (!generate_key(pk))
                {
                    ESP_LOGE(TAG, "TLS key generation failed");
                    mbedtls_pk_free(&pk);
                    return false;
                }
                new_key = true;
            }
            ESP_LOGI(TAG, "Issuing the self-signed certificate for %s", fqdn);

            uint8_t *buffer = static_cast<uint8_t *>(malloc(2 * MAX_DER));
            uint8_t *der = nullptr;
            const size_t cert_length = buffer == nullptr ? 0 : issue_certificate(pk, fqdn, buffer, MAX_DER, der);
            const int key_length = buffer == nullptr ? 0 : mbedtls_pk_write_key_der(&pk, buffer + MAX_DER, MAX_DER);
            if (cert_length == 0 || key_length <= 0)
            {
                free(buffer);
                mbedtls_pk_free(&pk);
                return false;
            }
            const uint8_t *key_der = buffer + 2 * MAX_DER - key_length;

            free(s_self_cert);
            free(s_self_key);
            s_self_cert = static_cast<uint8_t *>(malloc(cert_length));
            s_self_key = static_cast<uint8_t *>(malloc(key_length));
            if (s_self_cert == nullptr || s_self_key == nullptr)
            {
                memset(buffer, 0, 2 * MAX_DER);
                free(buffer);
                mbedtls_pk_free(&pk);
                return false;
            }
            memcpy(s_self_cert, der, cert_length);
            memcpy(s_self_key, key_der, key_length);
            s_self_cert_length = cert_length;
            s_self_key_length = static_cast<size_t>(key_length);
            memset(buffer, 0, 2 * MAX_DER);
            free(buffer);

            // With the saved settings lost (sys/record_mark.h) the FQDN is the default one: the identity is used, not
            // stored over the one that is left
            esp_err_t err = ESP_OK;
            if (web_settings_saved_lost())
                ESP_LOGW(TAG, "The saved web settings were lost: the certificate for %s is not stored", fqdn);
            else
            {
                err = new_key ? nvs_set_blob(handle, NVS_KEY_KEY, s_self_key, s_self_key_length) : ESP_OK;
                if (err == ESP_OK)
                    err = nvs_set_blob(handle, NVS_KEY_CERT, s_self_cert, s_self_cert_length);
                if (err == ESP_OK)
                    err = nvs_set_str(handle, NVS_KEY_NAME, fqdn);
                if (err == ESP_OK)
                    err = nvs_commit(handle);
            }
            if (err != ESP_OK)
                ESP_LOGW(TAG, "TLS identity could not be stored in NVS (%s): a new one is created at the next start", esp_err_to_name(err));
        }
        mbedtls_pk_free(&pk);
        return fingerprint(s_self_cert, s_self_cert_length, s_self_fingerprint);
    }
} // namespace

bool web_tls_load_or_create(const char *fqdn, web_tls_identity_t &identity)
{
    identity = web_tls_identity_t{};
    psa_crypto_init();

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS namespace %s unavailable (%s)", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    if (!load_or_create_self(handle, fqdn))
    {
        nvs_close(handle);
        return false;
    }
    // The own identity, checked again: a stored one that does not pass is not used
    uint8_t mode = 0;
    nvs_get_u8(handle, NVS_KEY_MODE, &mode);
    load_own(handle);
    nvs_close(handle);
    if (s_own_cert != nullptr)
    {
        web_tls_info_t info{};
        char check_err[160] = "";
        if (check(s_own_cert, s_own_key, info, check_err, sizeof(check_err)))
        {
            snprintf(s_own_fingerprint, sizeof(s_own_fingerprint), "%s", info.fingerprint);
            s_own_info = info;
        }
        else
        {
            ESP_LOGW(TAG, "Own certificate not used: %s", check_err);
            free_secret(s_own_cert);
            free_secret(s_own_key);
        }
    }
    s_own_mode = mode == 1 && s_own_cert != nullptr;
    if (mode == 1 && !s_own_mode)
        ESP_LOGW(TAG, "The own certificate is selected but missing: the self-signed one is used");
    fill(identity);
    return true;
}

bool web_tls_check_own(const char *cert_pem, const char *key_pem, web_tls_info_t &info, char *err, size_t err_size)
{
    return check(cert_pem, key_pem != nullptr && key_pem[0] != '\0' ? key_pem : s_own_key, info, err, err_size);
}

bool web_tls_set(bool own, const char *cert_pem, const char *key_pem, web_tls_identity_t &identity, char *err, size_t err_size)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    {
        snprintf(err, err_size, "The certificate could not be stored (NVS)");
        return false;
    }
    esp_err_t result = ESP_OK;
    char *new_cert = nullptr;
    char *new_key = nullptr;
    bool pair_stored = false;
    web_tls_info_t *info = own ? new (std::nothrow) web_tls_info_t{} : nullptr; // about 1 kB: not on the task stack
    if (own && info == nullptr)
    {
        nvs_close(handle);
        snprintf(err, err_size, "Out of memory");
        return false;
    }
    if (own)
    {
        const char *cert = cert_pem != nullptr && cert_pem[0] != '\0' ? cert_pem : s_own_cert;
        const char *key = key_pem != nullptr && key_pem[0] != '\0' ? key_pem : s_own_key;
        if (!check(cert, key, *info, err, err_size))
        {
            delete info;
            nvs_close(handle);
            return false;
        }
        // The stored pair again (a switch back to it, or the same texts once more): nothing is written, unless NVS
        // holds another pair since a store failed after its pair was stored
        const bool changed = s_own_differs || s_own_cert == nullptr || s_own_key == nullptr || strcmp(cert, s_own_cert) != 0 || strcmp(key, s_own_key) != 0;
        new_cert = copy_text(cert);
        new_key = copy_text(key);
        result = new_cert == nullptr || new_key == nullptr ? ESP_ERR_NO_MEM : ESP_OK;
        if (result == ESP_OK && changed)
        {
            bool room = true;
            result = store_pair(handle, new_cert, new_key, pair_stored, room, err, err_size);
            // Refused for lack of room before anything is written: NVS stays as it is
            if (!room)
            {
                delete info;
                free_secret(new_cert);
                free_secret(new_key);
                nvs_close(handle);
                return false;
            }
        }
    }
    if (result == ESP_OK)
        result = nvs_set_u8(handle, NVS_KEY_MODE, own ? 1 : 0);
    if (result == ESP_OK)
        result = nvs_commit(handle);
    nvs_close(handle);
    if (result != ESP_OK)
    {
        // The buffers in use stay (new connections still get them); the stored pair is complete either way
        if (pair_stored)
        {
            s_own_differs = true;
            ESP_LOGW(TAG, "The new own certificate is stored, a later step is not (%s): the next start uses it with the mode stored before", esp_err_to_name(result));
        }
        delete info;
        free_secret(new_cert);
        free_secret(new_key);
        snprintf(err, err_size, "The certificate could not be stored (NVS: %s)", esp_err_to_name(result));
        return false;
    }
    if (own)
    {
        // The old buffers are no longer used: a TLS session copies them when it starts, in this same task
        free_secret(s_own_cert);
        free_secret(s_own_key);
        s_own_cert = new_cert;
        s_own_key = new_key;
        s_own_info = *info;
        snprintf(s_own_fingerprint, sizeof(s_own_fingerprint), "%s", info->fingerprint);
        delete info;
        s_own_differs = false;
    }
    s_own_mode = own;
    fill(identity);
    ESP_LOGI(TAG, "Web admin certificate: %s, SHA-256 %s", own ? "own" : "self-signed", identity.fingerprint);
    return true;
}

bool web_tls_own_info(web_tls_info_t &info)
{
    if (s_own_cert == nullptr)
        return false;
    info = s_own_info;
    return true;
}

const char *web_tls_own_certificate_pem()
{
    return s_own_cert;
}

const char *web_tls_own_key_pem()
{
    return s_own_key;
}

void web_tls_clear_own()
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return;
    own_nvs_t o;
    if (!read_own(handle, o))
    {
        // Not known for sure which items exist: all of them, in the order of the rule without equal halves
        o.items.ocrt = o.items.opair = o.items.opair2 = true;
        o.items.okey = web_tls_okey_t::key;
        o.items.ocrt_in = o.items.okey_in = 0;
    }
    // In the order of the rule (each step leaves the pair before or none); it stops at the first error
    esp_err_t err = ESP_OK;
    const web_tls_own_plan_t plan = web_tls_own_clear(o.items);
    if (run_steps(handle, plan, o, nullptr, 0, err) < plan.count)
        ESP_LOGW(TAG, "Own certificate: not all of it could be erased (%s)", esp_err_to_name(err));
    free_own(o);
    nvs_erase_key(handle, NVS_KEY_MODE);
    nvs_commit(handle);
    nvs_close(handle);
}
