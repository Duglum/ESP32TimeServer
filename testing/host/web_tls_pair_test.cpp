// ESP32 Time Server host tests: the stored form of the own web certificate and the NVS room check
// (main/web/web_tls_pair.cpp)
#define TEST_NAME "web_tls_pair_test"
#include "check.h"

#include "web/web_tls_pair.h"

#include <string>
#include <vector>

namespace
{
    bool split(const std::vector<uint8_t> &blob, std::string &cert, std::string &key)
    {
        const char *c = nullptr;
        const char *k = nullptr;
        if (!web_tls_pair_split(blob.data(), blob.size(), c, k))
            return false;
        cert = c;
        key = k;
        return true;
    }

    std::vector<uint8_t> bytes(const char *text, size_t size)
    {
        return std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(text), reinterpret_cast<const uint8_t *>(text) + size);
    }

    void test_pack_split()
    {
        std::vector<uint8_t> out(32, 0xAA);
        CHECK(web_tls_pair_pack("CERT", "KEY", out.data(), out.size()) == 9);
        CHECK(memcmp(out.data(), "CERT\0KEY\0", 9) == 0);
        CHECK(out[9] == 0xAA);
        out.resize(9);
        std::string cert, key;
        CHECK(split(out, cert, key));
        CHECK_STR(cert.c_str(), "CERT");
        CHECK_STR(key.c_str(), "KEY");
        // Too small, empty texts, null
        std::vector<uint8_t> small(8);
        CHECK(web_tls_pair_pack("CERT", "KEY", small.data(), small.size()) == 0);
        CHECK(web_tls_pair_pack("", "KEY", out.data(), out.size()) == 0);
        CHECK(web_tls_pair_pack("CERT", "", out.data(), out.size()) == 0);
        CHECK(web_tls_pair_pack(nullptr, "KEY", out.data(), out.size()) == 0);
        CHECK(web_tls_pair_pack("CERT", "KEY", nullptr, 100) == 0);
        CHECK(web_tls_pair_size(8192, 4096) == 12290);

        // The largest pair: round trip
        std::string big_cert(8192, 'c'), big_key(4096, 'k');
        std::vector<uint8_t> big(web_tls_pair_size(big_cert.size(), big_key.size()));
        CHECK(web_tls_pair_pack(big_cert.c_str(), big_key.c_str(), big.data(), big.size()) == big.size());
        CHECK(split(big, cert, key));
        CHECK(cert == big_cert && key == big_key);
    }

    void test_split_refused()
    {
        std::string cert, key;
        CHECK(!split(bytes("CERT\0KEY", 8), cert, key));         // no NUL at the end
        CHECK(!split(bytes("CERT\0", 5), cert, key));             // no key
        CHECK(!split(bytes("CERT\0\0", 6), cert, key));           // empty key
        CHECK(!split(bytes("\0KEY\0", 5), cert, key));            // empty chain
        CHECK(!split(bytes("CERT\0K\0Y\0", 9), cert, key));       // a third text
        CHECK(!split(bytes("CERTKEY\0", 8), cert, key));          // one text only
        CHECK(!split(bytes("", 0), cert, key));
        CHECK(split(bytes("C\0K\0", 4), cert, key));
        const char *c = "x";
        const char *k = "y";
        CHECK(!web_tls_pair_split(nullptr, 4, c, k) && c == nullptr && k == nullptr);
    }

    void test_entries()
    {
        // Measured on the board (NVS listings and used entries, 2026-11-19): a blob takes at most the estimate and at
        // least the lower bound. ocrt 6001 bytes in 3 chunks: 192 entries; okey 1709 bytes in 2 chunks: 57; ocrt
        // 1706 bytes in 1 chunk: 56; crt 472 bytes: 17; key 121 bytes: 6; opair 2387 bytes in 2 chunks: 78; opair
        // 3415 bytes in 2 chunks: 110; opair 12290 bytes: 390 (used entries before and after).
        const struct
        {
            size_t size, measured;
        } stored[] = {{6001, 192}, {1709, 57}, {1706, 56}, {472, 17}, {121, 6}, {2387, 78}, {3415, 110}, {12290, 390}};
        for (const auto &s : stored)
        {
            CHECK(web_tls_nvs_entries_max(s.size) >= s.measured);
            CHECK(web_tls_nvs_entries_min(s.size) <= s.measured);
        }
        CHECK(web_tls_nvs_entries_max(12290) == 385 + 4 + 5 + 1);
        CHECK(web_tls_nvs_entries_min(12290) == 387);
        CHECK(web_tls_nvs_entries_max(6996) == 219 + 2 + 3 + 1);
        // Worst case of the split: every chunk rounded up, one chunk more than size / 4000
        for (size_t size = 1; size <= 20000; ++size)
        {
            const size_t chunks = (size + 3999) / 4000 + 1;
            CHECK(web_tls_nvs_entries_max(size) >= (size + 31) / 32 + chunks - 1 + chunks + 1);
            CHECK(web_tls_nvs_entries_min(size) <= web_tls_nvs_entries_max(size));
        }
    }

    void test_room()
    {
        const size_t reserve = web_tls_nvs_entries_max(6996); // 225
        size_t needed = 0;
        // Old table with the stored state of the test board: 253 available, the TEST pair (ocrt 1706 + okey 1709)
        const size_t old = web_tls_nvs_entries_min(1706) + web_tls_nvs_entries_min(1709);
        CHECK(old == 112);
        // Chain 6000 + key 4096 (fix 8's case): refused before anything is written
        CHECK(!web_tls_nvs_room(253, web_tls_pair_size(6000, 4096), old, reserve, needed));
        CHECK(needed == web_tls_nvs_entries_max(10098) - old + reserve);
        // The largest pair: refused
        CHECK(!web_tls_nvs_room(253, web_tls_pair_size(8192, 4096), old, reserve, needed));
        // The TEST pair again in the new layout: fits
        CHECK(web_tls_nvs_room(253, web_tls_pair_size(1705, 1708), old, reserve, needed));
        // A pair that does not grow the data needs only the room next to the old data, not the reserve
        CHECK(web_tls_nvs_room(120, web_tls_pair_size(1705, 1708), old, reserve, needed) && needed == web_tls_nvs_entries_max(3415));
        CHECK(!web_tls_nvs_room(110, web_tls_pair_size(1705, 1708), old, reserve, needed) && needed == web_tls_nvs_entries_max(3415));
        // No old data: the new pair and the reserve
        CHECK(!web_tls_nvs_room(335, web_tls_pair_size(1705, 1708), 0, reserve, needed) && needed == 111 + 225);
        CHECK(web_tls_nvs_room(336, web_tls_pair_size(1705, 1708), 0, reserve, needed));
        // New 256 kB table (7561 available with the stored state): the largest pair fits
        CHECK(web_tls_nvs_room(7561, web_tls_pair_size(8192, 4096), old, reserve, needed));
    }
} // namespace

void run_tests()
{
    test_pack_split();
    test_split_refused();
    test_entries();
    test_room();
}

// Any blob: a split that succeeds gives two non-empty texts that pack to the same bytes
void fuzz_one(const uint8_t *data, size_t size)
{
    const char *cert = nullptr;
    const char *key = nullptr;
    if (!web_tls_pair_split(data, size, cert, key))
        return;
    CHECK(cert[0] != '\0' && key[0] != '\0');
    std::vector<uint8_t> again(size);
    CHECK(web_tls_pair_pack(cert, key, again.data(), again.size()) == size);
    CHECK(memcmp(again.data(), data, size) == 0);
}
