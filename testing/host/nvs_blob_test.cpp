// ESP32 Time Server host tests: an NVS blob is written only when its bytes differ from the stored ones
// (main/custom/nvs_blob.cpp, the workaround for the multi-chunk compare of ESP-IDF 6.1.1 nvs_set_blob())
#define TEST_NAME "nvs_blob_test"
#include "check.h"

#include "custom/nvs_blob.h"

#include <map>
#include <string>
#include <vector>

namespace
{
    // A fake NVS with the behaviour of ESP-IDF 6.1.1 for a blob of more than one chunk: nvs_set_blob() always writes
    // the new version, and needs room for it next to the old one (the old one is erased only afterwards); a failed
    // write leaves the old version. nvs_get_blob() as ESP-IDF: nullptr gives the size, a short buffer
    // ESP_ERR_NVS_INVALID_LENGTH.
    struct fake_nvs_t
    {
        std::map<std::string, std::vector<uint8_t>> blobs;
        size_t capacity = 1u << 20;  // bytes for all blobs, the new version included while it is written
        int sets = 0;                // nvs_set_blob() calls
        int writes = 0;              // of them: written
        int gets = 0;                // nvs_get_blob() calls
        esp_err_t size_error = ESP_OK; // returned by the size query (nullptr buffer), if not ESP_OK
        esp_err_t read_error = ESP_OK; // returned by the read into a buffer, if not ESP_OK
        int flip_on_read = -1;       // the read copies the stored bytes with this byte inverted (>= 0)

        size_t used() const
        {
            size_t n = 0;
            for (const auto &b : blobs)
                n += b.second.size();
            return n;
        }
    };
    fake_nvs_t g_nvs;

    void reset(size_t capacity = 1u << 20)
    {
        g_nvs = fake_nvs_t{};
        g_nvs.capacity = capacity;
    }

    std::vector<uint8_t> pattern(size_t size, uint8_t seed)
    {
        std::vector<uint8_t> v(size);
        for (size_t i = 0; i < size; ++i)
            v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
        return v;
    }

    // The settings blob (settings_blob_t, 6996 bytes, 3 chunks on the board)
    constexpr size_t SETTINGS = 6996;
    constexpr nvs_handle_t HANDLE = 7;

    esp_err_t store(const char *key, const std::vector<uint8_t> &data, bool *written)
    {
        return nvs_blob_set_if_changed(HANDLE, key, data.data(), data.size(), written);
    }

    void test_new_and_same()
    {
        reset();
        const std::vector<uint8_t> a = pattern(SETTINGS, 1);
        bool written = false;
        CHECK(store("saved", a, &written) == ESP_OK && written);
        CHECK(g_nvs.sets == 1 && g_nvs.blobs["saved"] == a);
        // The same bytes three times (three Saves of unchanged settings): nothing written
        for (int i = 0; i < 3; ++i)
        {
            written = true;
            CHECK(store("saved", a, &written) == ESP_OK && !written);
        }
        CHECK(g_nvs.sets == 1 && g_nvs.writes == 1);
        // A copy in another buffer is the same too
        const std::vector<uint8_t> copy = a;
        CHECK(store("saved", copy, &written) == ESP_OK && !written && g_nvs.sets == 1);
        // written may be nullptr
        CHECK(store("saved", a, nullptr) == ESP_OK && g_nvs.sets == 1);
        // Another key with the same bytes is its own item
        CHECK(store("once", a, &written) == ESP_OK && written && g_nvs.sets == 2);
        CHECK(store("once", a, &written) == ESP_OK && !written && g_nvs.sets == 2);
    }

    void test_changed()
    {
        // One byte changed at the start, in each chunk of the board layout (832 / 3936 / 2228 bytes) and at the end
        for (size_t at : {size_t(0), size_t(500), size_t(831), size_t(832), size_t(3000), size_t(4767), size_t(4768), size_t(SETTINGS - 1)})
        {
            reset();
            std::vector<uint8_t> a = pattern(SETTINGS, 2);
            bool written = false;
            CHECK(store("saved", a, &written) == ESP_OK && written);
            a[at] ^= 0x01;
            CHECK(store("saved", a, &written) == ESP_OK && written);
            CHECK(g_nvs.sets == 2 && g_nvs.blobs["saved"] == a);
        }
        // Other size: shorter, longer (the stored one is not read into a buffer of the wrong size)
        reset();
        bool written = false;
        std::vector<uint8_t> a = pattern(SETTINGS, 3);
        CHECK(store("saved", a, &written) == ESP_OK && written);
        std::vector<uint8_t> shorter(a.begin(), a.end() - 1);
        CHECK(store("saved", shorter, &written) == ESP_OK && written && g_nvs.blobs["saved"] == shorter);
        CHECK(store("saved", a, &written) == ESP_OK && written && g_nvs.blobs["saved"] == a);
        CHECK(g_nvs.sets == 3);
        // Size 0 goes to nvs_set_blob() as before
        CHECK(nvs_blob_set_if_changed(HANDLE, "empty", a.data(), 0, &written) == ESP_OK && written && g_nvs.sets == 4);
        // A one-chunk blob behaves the same (156 bytes, the allow list)
        const std::vector<uint8_t> small = pattern(156, 4);
        CHECK(store("allow", small, &written) == ESP_OK && written);
        CHECK(store("allow", small, &written) == ESP_OK && !written && g_nvs.sets == 5);
    }

    void test_read_errors()
    {
        // The size query fails (other than not found): nvs_set_blob() decides, its result is returned
        reset();
        const std::vector<uint8_t> a = pattern(SETTINGS, 5);
        bool written = false;
        CHECK(store("saved", a, &written) == ESP_OK && written);
        g_nvs.size_error = ESP_ERR_NVS_INVALID_HANDLE;
        CHECK(store("saved", a, &written) == ESP_OK && written && g_nvs.sets == 2);
        // The read into the copy fails: written as before
        reset();
        CHECK(store("saved", a, &written) == ESP_OK && written);
        g_nvs.read_error = ESP_ERR_NVS_INVALID_LENGTH;
        CHECK(store("saved", a, &written) == ESP_OK && written && g_nvs.sets == 2);
        // The read gives other bytes than the caller's (as if NVS held a changed set): written
        reset();
        CHECK(store("saved", a, &written) == ESP_OK && written);
        g_nvs.flip_on_read = 4000;
        CHECK(store("saved", a, &written) == ESP_OK && written && g_nvs.sets == 2);
    }

    void test_full()
    {
        // Room for one settings blob and a bit more, not for a second copy (fix 8's full table)
        reset(SETTINGS + 1000);
        const std::vector<uint8_t> a = pattern(SETTINGS, 6);
        bool written = false;
        CHECK(store("saved", a, &written) == ESP_OK && written);
        // Unchanged: ESP_OK, nothing written (6.1.1 alone: ESP_ERR_NVS_NOT_ENOUGH_SPACE, the "500" of /api/save)
        CHECK(store("saved", a, &written) == ESP_OK && !written);
        g_nvs.sets = 0;
        CHECK(nvs_set_blob(HANDLE, "saved", a.data(), a.size()) == ESP_ERR_NVS_NOT_ENOUGH_SPACE); // the fake's fault model
        // Changed: refused, the stored set stays as it was
        std::vector<uint8_t> b = a;
        b[100] ^= 0xFF;
        CHECK(store("saved", b, &written) == ESP_ERR_NVS_NOT_ENOUGH_SPACE && !written);
        CHECK(g_nvs.blobs["saved"] == a);
        // And the stored set can still be saved again
        CHECK(store("saved", a, &written) == ESP_OK && !written);
    }
} // namespace

extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    ++g_nvs.sets;
    if (handle != HANDLE || key == nullptr || (value == nullptr && length > 0))
        return ESP_ERR_NVS_INVALID_HANDLE;
    // The new version is written next to the old one, which is erased afterwards
    if (g_nvs.used() + length > g_nvs.capacity)
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    const uint8_t *p = static_cast<const uint8_t *>(value);
    g_nvs.blobs[key] = std::vector<uint8_t>(p, p + length);
    ++g_nvs.writes;
    return ESP_OK;
}

extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    ++g_nvs.gets;
    if (handle != HANDLE || key == nullptr || length == nullptr)
        return ESP_ERR_NVS_INVALID_HANDLE;
    const auto it = g_nvs.blobs.find(key);
    if (it == g_nvs.blobs.end())
        return ESP_ERR_NVS_NOT_FOUND;
    if (out_value == nullptr)
    {
        if (g_nvs.size_error != ESP_OK)
            return g_nvs.size_error;
        *length = it->second.size();
        return ESP_OK;
    }
    if (g_nvs.read_error != ESP_OK)
        return g_nvs.read_error;
    if (*length < it->second.size())
        return ESP_ERR_NVS_INVALID_LENGTH;
    if (!it->second.empty())
        memcpy(out_value, it->second.data(), it->second.size());
    if (g_nvs.flip_on_read >= 0 && static_cast<size_t>(g_nvs.flip_on_read) < it->second.size())
        static_cast<uint8_t *>(out_value)[g_nvs.flip_on_read] ^= 0xFF;
    *length = it->second.size();
    return ESP_OK;
}

void run_tests()
{
    test_new_and_same();
    test_changed();
    test_read_errors();
    test_full();
}

// The input as the stored blob and the new one (first byte: where to split): written exactly when they differ, and
// afterwards NVS holds the new bytes
void fuzz_one(const uint8_t *data, size_t size)
{
    if (size == 0)
        return;
    const size_t cut = 1 + (data[0] % size);
    const std::vector<uint8_t> stored(data + 1, data + cut);
    const std::vector<uint8_t> next(data + cut, data + size);
    reset();
    if (!stored.empty())
        g_nvs.blobs["k"] = stored;
    bool written = false;
    CHECK(store("k", next, &written) == ESP_OK);
    CHECK(written == (stored.empty() || next.empty() || stored != next));
    CHECK(g_nvs.blobs["k"] == next);
}
