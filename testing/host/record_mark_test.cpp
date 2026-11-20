// ESP32-WebTherm host tests: the presence marks of stored security records (main/sys/record_mark_core.cpp and
// record_mark.cpp against a fake NVS), and that the write order of the firmware cannot leave a mark without its record
// when the device stops at any write.
#define TEST_NAME "record_mark_test"
#include "check.h"

#include "nvs.h"
#include "sys/record_mark.h"

#include <map>
#include <string>
#include <vector>

namespace
{
    // A fake NVS: namespaces of u64 items, and "records" (the blobs of the firmware, written by the test itself). Every
    // write (an item set or erased, a namespace made by NVS_READWRITE) counts; after writes_left reaches 0 the device
    // has stopped: nothing is written any more, every call fails.
    struct fake_nvs_t
    {
        std::map<std::string, std::map<std::string, uint64_t>> items;
        std::map<std::string, bool> type_mismatch;     // "<ns>/<key>": nvs_get_u64 answers ESP_ERR_NVS_TYPE_MISMATCH
        esp_err_t open_error = ESP_OK;                 // nvs_open answers this (not ESP_OK)
        int writes = 0;
        int writes_left = -1; // -1: no limit
        uint32_t next_handle = 1;
        std::map<uint32_t, std::string> handles;
    };
    fake_nvs_t g;

    void reset()
    {
        g = fake_nvs_t{};
    }

    bool write_allowed()
    {
        if (g.writes_left == 0)
            return false;
        if (g.writes_left > 0)
            --g.writes_left;
        ++g.writes;
        return true;
    }

    // The test's own records (blobs): present or not
    std::map<std::string, bool> g_records;

    bool record_write(const std::string &name)
    {
        if (!write_allowed())
            return false;
        g_records[name] = true;
        return true;
    }

    bool record_erase(const std::string &name)
    {
        if (!write_allowed())
            return false;
        g_records.erase(name);
        return true;
    }

    bool record(const std::string &name)
    {
        return g_records.count(name) != 0;
    }

    std::string ns_of(nvs_handle_t handle)
    {
        const auto it = g.handles.find(handle);
        return it != g.handles.end() ? it->second : std::string();
    }
} // namespace

extern "C"
{
    esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
    {
        if (g.open_error != ESP_OK)
            return g.open_error;
        if (g.items.count(namespace_name) == 0)
        {
            if (open_mode == NVS_READONLY)
                return ESP_ERR_NVS_NOT_FOUND;
            if (!write_allowed())
                return ESP_FAIL;
            g.items[namespace_name];
        }
        *out_handle = g.next_handle++;
        g.handles[*out_handle] = namespace_name;
        return ESP_OK;
    }

    void nvs_close(nvs_handle_t handle)
    {
        g.handles.erase(handle);
    }

    esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *out_value)
    {
        const std::string ns = ns_of(handle);
        if (g.type_mismatch.count(ns + "/" + key))
            return ESP_ERR_NVS_TYPE_MISMATCH;
        const auto &items = g.items[ns];
        const auto it = items.find(key);
        if (it == items.end())
            return ESP_ERR_NVS_NOT_FOUND;
        *out_value = it->second;
        return ESP_OK;
    }

    esp_err_t nvs_set_u64(nvs_handle_t handle, const char *key, uint64_t value)
    {
        if (!write_allowed())
            return ESP_FAIL;
        g.items[ns_of(handle)][key] = value;
        return ESP_OK;
    }

    esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
    {
        auto &items = g.items[ns_of(handle)];
        if (items.count(key) == 0)
            return ESP_ERR_NVS_NOT_FOUND;
        if (!write_allowed())
            return ESP_FAIL;
        items.erase(key);
        return ESP_OK;
    }

    esp_err_t nvs_erase_all(nvs_handle_t handle)
    {
        auto &items = g.items[ns_of(handle)];
        // NVS erases item by item: a stop can come after any of them
        while (!items.empty())
        {
            if (!write_allowed())
                return ESP_FAIL;
            items.erase(items.begin());
        }
        return ESP_OK;
    }

    esp_err_t nvs_commit(nvs_handle_t)
    {
        return g.writes_left == 0 ? ESP_FAIL : ESP_OK;
    }

    // Not used by record_mark.cpp; declared by the shared stub
    esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t)
    {
        return ESP_FAIL;
    }

    esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *)
    {
        return ESP_FAIL;
    }
}

namespace
{
    bool marked(const char *ns, const char *key, uint64_t *value = nullptr)
    {
        char mark[RECORD_MARK_KEY_SIZE];
        if (!record_mark_key(ns, key, mark))
            return false;
        const auto &items = g.items[RECORD_MARK_NVS_NAMESPACE];
        const auto it = items.find(mark);
        if (it == items.end())
            return false;
        if (value != nullptr)
            *value = it->second;
        return true;
    }

    void test_decide()
    {
        CHECK(record_mark_decide(false, false) == record_mark_state_t::none);
        CHECK(record_mark_decide(true, true) == record_mark_state_t::present);
        CHECK(record_mark_decide(false, true) == record_mark_state_t::adopted);
        CHECK(record_mark_decide(true, false) == record_mark_state_t::lost);
    }

    void test_key_and_hash()
    {
        char out[RECORD_MARK_KEY_SIZE];
        CHECK(record_mark_key("webauth", "pw", out));
        CHECK_STR(out, "webauth/pw");
        // The marks of the firmware (web_auth.cpp, web_settings.cpp, totp_store.cpp) fit
        CHECK(record_mark_key("webauth", "allow", out));
        CHECK_STR(out, "webauth/allow");
        CHECK(record_mark_key("webcfg", "saved", out));
        CHECK_STR(out, "webcfg/saved");
        CHECK(record_mark_key("totp", "t16", out));
        CHECK_STR(out, "totp/t16");
        CHECK(record_mark_key("abcdefg", "1234567", out)); // 15 characters
        CHECK_STR(out, "abcdefg/1234567");
        CHECK(!record_mark_key("abcdefg", "12345678", out)); // 16
        CHECK_STR(out, "");
        CHECK(!record_mark_key("", "pw", out));
        CHECK(!record_mark_key("webauth", "", out));
        CHECK(!record_mark_key(nullptr, "pw", out));
        CHECK(!record_mark_key("webauth", nullptr, out));
        // FNV-1a 64 test vectors
        CHECK(record_mark_text_hash("") == 0xcbf29ce484222325ull);
        CHECK(record_mark_text_hash("a") == 0xaf63dc4c8601ec8cull);
        CHECK(record_mark_text_hash("foobar") == 0x85944171f73967e8ull);
        CHECK(record_mark_text_hash(nullptr) == 0xcbf29ce484222325ull);
        CHECK(record_mark_text_hash("admin") != record_mark_text_hash("admin2"));
    }

    void test_check_and_set()
    {
        reset();
        // Never stored: none, and nothing is written (no namespace made)
        CHECK(record_mark_check("webauth", "pw", false, 1, nullptr) == record_mark_state_t::none);
        CHECK(g.writes == 0);
        CHECK(g.items.count(RECORD_MARK_NVS_NAMESPACE) == 0);
        // A record without a mark (stored before the marks): adopted, the mark is set
        uint64_t stored = 7;
        CHECK(record_mark_check("webauth", "pw", true, 1, &stored) == record_mark_state_t::adopted);
        CHECK(stored == 0);
        uint64_t value = 0;
        CHECK(marked("webauth", "pw", &value) && value == 1);
        // Both: present, nothing written
        int writes = g.writes;
        CHECK(record_mark_check("webauth", "pw", true, 1, &stored) == record_mark_state_t::present);
        CHECK(stored == 1);
        CHECK(g.writes == writes);
        // present with another value (a token's user): the mark is corrected
        CHECK(record_mark_check("webauth", "pw", true, 42, &stored) == record_mark_state_t::present);
        CHECK(stored == 1);
        CHECK(marked("webauth", "pw", &value) && value == 42);
        // The record lost: lost, the value of the mark given back, nothing written
        writes = g.writes;
        CHECK(record_mark_check("webauth", "pw", false, 0, &stored) == record_mark_state_t::lost);
        CHECK(stored == 42);
        CHECK(g.writes == writes);
        CHECK(marked("webauth", "pw"));

        // record_mark_set writes only when missing or different
        writes = g.writes;
        CHECK(record_mark_set("webauth", "pw", 42) == ESP_OK);
        CHECK(g.writes == writes);
        CHECK(record_mark_set("webauth", "pw", 43) == ESP_OK);
        CHECK(g.writes == writes + 1);
        CHECK(record_mark_set("webauth", "a-key-far-too-long", 1) == ESP_ERR_INVALID_ARG);

        // record_mark_clear: the mark goes; a second clear and a clear of a mark never set are ESP_OK without a write
        CHECK(record_mark_clear("webauth", "pw") == ESP_OK);
        CHECK(!marked("webauth", "pw"));
        writes = g.writes;
        CHECK(record_mark_clear("webauth", "pw") == ESP_OK);
        CHECK(record_mark_clear("totp", "t3") == ESP_OK);
        CHECK(g.writes == writes);
        reset();
        CHECK(record_mark_clear("totp", "t3") == ESP_OK); // without the namespace: none is made
        CHECK(g.items.count(RECORD_MARK_NVS_NAMESPACE) == 0);

        // record_mark_clear_all
        reset();
        CHECK(record_mark_clear_all() == ESP_OK);
        CHECK(g.items.count(RECORD_MARK_NVS_NAMESPACE) == 0);
        CHECK(record_mark_set("webauth", "pw", 1) == ESP_OK);
        CHECK(record_mark_set("webcfg", "saved", 1) == ESP_OK);
        CHECK(record_mark_set("totp", "t0", 9) == ESP_OK);
        CHECK(record_mark_clear_all() == ESP_OK);
        CHECK(!marked("webauth", "pw") && !marked("webcfg", "saved") && !marked("totp", "t0"));

        // A mark that cannot be read counts as a mark (a record that is not valid then is lost); NVS not usable at all
        // (not initialised, no partition): no mark, nothing to compare
        reset();
        CHECK(record_mark_set("webauth", "allow", 1) == ESP_OK);
        g.type_mismatch["recmark/webauth/allow"] = true;
        CHECK(record_mark_check("webauth", "allow", false, 0, nullptr) == record_mark_state_t::lost);
        CHECK(record_mark_check("webauth", "allow", true, 1, nullptr) == record_mark_state_t::present);
        g.open_error = ESP_ERR_NVS_NOT_INITIALIZED;
        CHECK(record_mark_check("webauth", "allow", false, 0, nullptr) == record_mark_state_t::none);
        g.open_error = ESP_ERR_NVS_PART_NOT_FOUND;
        CHECK(record_mark_check("webauth", "allow", false, 0, nullptr) == record_mark_state_t::none);
        g.open_error = ESP_OK;
    }

    // The device stops after k writes, for every k, then starts again (record_mark_check of every record): never lost.
    // setup: the state before; steps: the writes in the firmware's order; records: the records it touches.
    struct record_ref_t
    {
        const char *ns;
        const char *key;
    };

    int lost_after_cuts(void (*setup)(), void (*steps)(), const std::vector<record_ref_t> &records)
    {
        // How many writes the steps make without a stop
        reset();
        g_records.clear();
        setup();
        const int before = g.writes;
        steps();
        const int total = g.writes - before;
        int lost = 0;
        for (int k = 0; k <= total; ++k)
        {
            reset();
            g_records.clear();
            setup();
            g.writes_left = k;
            steps();
            g.writes_left = -1; // the next start
            for (const record_ref_t &r : records)
                if (record_mark_check(r.ns, r.key, record(std::string(r.ns) + "/" + r.key), 1, nullptr) == record_mark_state_t::lost)
                    ++lost;
        }
        return lost;
    }

    void nothing()
    {
    }

    void stored_pw()
    {
        record_write("webauth/pw");
        record_mark_set("webauth", "pw", 1);
    }

    // web_auth_set_password(), web_auth_set_allow_list(), web_settings_save(), the save of a TOTP token
    void store_pw()
    {
        if (record_write("webauth/pw"))
            record_mark_set("webauth", "pw", 1);
    }

    // The erase of a TOTP token (totp_store.cpp, erase())
    void erase_pw()
    {
        if (record_mark_clear("webauth", "pw") == ESP_OK)
            record_erase("webauth/pw");
    }

    // The wrong order, to show that the test sees it
    void erase_pw_wrong_order()
    {
        if (record_erase("webauth/pw"))
            record_mark_clear("webauth", "pw");
    }

    void stored_all()
    {
        for (const char *key : {"webauth/pw", "webauth/allow", "webcfg/saved", "totp/t0"})
            record_write(key);
        record_mark_set("webauth", "pw", 1);
        record_mark_set("webauth", "allow", 1);
        record_mark_set("webcfg", "saved", 1);
        record_mark_set("totp", "t0", 5);
    }

    // web_settings_factory_reset(): every mark first, then the records namespace by namespace
    void factory_reset()
    {
        if (record_mark_clear_all() != ESP_OK)
            return;
        for (const char *key : {"webcfg/saved", "webauth/pw", "webauth/allow", "totp/t0"})
            if (!record_erase(key))
                return;
    }

    void factory_reset_wrong_order()
    {
        for (const char *key : {"webcfg/saved", "webauth/pw", "webauth/allow", "totp/t0"})
            if (!record_erase(key))
                return;
        record_mark_clear_all();
    }

    void test_cuts()
    {
        const std::vector<record_ref_t> pw = {{"webauth", "pw"}};
        const std::vector<record_ref_t> all = {{"webauth", "pw"}, {"webauth", "allow"}, {"webcfg", "saved"}, {"totp", "t0"}};
        CHECK(lost_after_cuts(nothing, store_pw, pw) == 0);        // the first store
        CHECK(lost_after_cuts(stored_pw, store_pw, pw) == 0);      // a change
        CHECK(lost_after_cuts(stored_pw, erase_pw, pw) == 0);      // an erase
        CHECK(lost_after_cuts(stored_all, factory_reset, all) == 0);
        CHECK(lost_after_cuts(nothing, factory_reset, all) == 0);
        // The test sees a wrong order
        CHECK(lost_after_cuts(stored_pw, erase_pw_wrong_order, pw) > 0);
        CHECK(lost_after_cuts(stored_all, factory_reset_wrong_order, all) > 0);
        // A real loss is seen
        reset();
        g_records.clear();
        stored_pw();
        g_records.clear();
        CHECK(record_mark_check("webauth", "pw", record("webauth/pw"), 1, nullptr) == record_mark_state_t::lost);
    }
} // namespace

void run_tests()
{
    test_decide();
    test_key_and_hash();
    test_check_and_set();
    test_cuts();
}

void fuzz_one(const uint8_t *, size_t)
{
}
