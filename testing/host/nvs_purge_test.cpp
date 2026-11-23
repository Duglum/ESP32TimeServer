// ESP32-WebTherm host tests: the purge of erased NVS data at the start (main/sys/nvs_purge_core.cpp, nvs_purge.cpp).
// The scan of a page against pages built here in the format of ESP-IDF 6.1.1 (and a page header and a namespace entry
// read from the test board's NVS); nvs_purge_start() against a fake NVS partition: only erased entries that hold
// data are written, with zeros, everything else stays byte for byte; a second start writes nothing; a write that
// fails does not stop the other pages; nothing happens while NVS cannot be used.
#define TEST_NAME "nvs_purge_test"
#include "check.h"

#include "esp_partition.h"
#include "sys/nvs_purge.h"
#include "sys/nvs_state.h"

#include <set>
#include <string>
#include <vector>

namespace
{
    constexpr size_t PAGE = NVS_PURGE_PAGE_SIZE;
    constexpr size_t ENTRIES = 126;
    constexpr uint8_t ERASED = 0, ILLEGAL = 1, WRITTEN = 2, EMPTY = 3; // entry states of the bitmap

    uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t size)
    {
        crc = ~crc;
        while (size-- > 0)
        {
            crc ^= *data++;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    void put32(uint8_t *p, uint32_t v)
    {
        p[0] = v & 0xff;
        p[1] = (v >> 8) & 0xff;
        p[2] = (v >> 16) & 0xff;
        p[3] = v >> 24;
    }

    std::vector<uint8_t> bytes_of(const char *hex)
    {
        std::vector<uint8_t> out;
        for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0'; i += 2)
            out.push_back(static_cast<uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
        return out;
    }

    struct page_t
    {
        std::vector<uint8_t> b = std::vector<uint8_t>(PAGE, 0xff);

        explicit page_t(uint32_t state = 0xFFFFFFFCu, uint32_t seq = 7)
        {
            put32(&b[0], state);
            put32(&b[4], seq);
            b[8] = 0xfe; // version 2
            put32(&b[28], crc32_le(0xFFFFFFFFu, &b[4], 24));
        }
        uint8_t *entry(size_t i)
        {
            return &b[64 + i * 32];
        }
        void state(size_t i, uint8_t st)
        {
            uint8_t &w = b[32 + i / 4];
            w = static_cast<uint8_t>((w & ~(3u << ((i % 4) * 2))) | (st << ((i % 4) * 2)));
        }
        // Entries from..to-1 in state st, filled with fill (0x00: zeroed, as NVS's purge leaves them)
        void fill(size_t from, size_t to, uint8_t st, uint8_t fill)
        {
            for (size_t i = from; i < to; ++i)
            {
                memset(entry(i), fill, 32);
                state(i, st);
            }
        }
    };

    size_t runs_of(const page_t &p, std::vector<nvs_purge_run_t> &out)
    {
        nvs_purge_run_t runs[NVS_PURGE_MAX_RUNS];
        const size_t count = nvs_purge_page_runs(p.b.data(), runs);
        out.assign(runs, runs + count);
        return count;
    }

    bool run_is(const nvs_purge_run_t &r, size_t first, size_t count)
    {
        return r.offset == 64 + first * 32 && r.size == count * 32;
    }

    // ---- The scan of a page ----

    void test_real_page_header()
    {
        // Page 1 of the test board's NVS (2026-11-20, FULL, sequence 0x85) and its entry 0, the namespace "totp" as
        // ESP-IDF wrote them; then an erased entry with data
        page_t p;
        const std::vector<uint8_t> header = bytes_of("fcffffff85000000feffffffffffffffffffffffffffffffffffffff9623201c");
        const std::vector<uint8_t> entry = bytes_of("000101ffaadf6eac746f747000000000000000000000000004ffffffffffffff");
        memcpy(p.b.data(), header.data(), 32);
        memcpy(p.entry(0), entry.data(), 32);
        p.state(0, WRITTEN);
        p.fill(1, 2, ERASED, 0x5a);
        std::vector<nvs_purge_run_t> runs;
        CHECK(runs_of(p, runs) == 1 && run_is(runs[0], 1, 1));
        // One bit of the header changed: not a page NVS uses
        p.b[4] ^= 1;
        CHECK(runs_of(p, runs) == 0);
    }

    void test_page_states()
    {
        std::vector<nvs_purge_run_t> runs;
        page_t uninit;
        uninit.b.assign(PAGE, 0xff);
        CHECK(runs_of(uninit, runs) == 0);
        for (uint32_t state : {0xFFFFFFFEu, 0xFFFFFFFCu, 0xFFFFFFF8u, 0xFFFFFFF0u, 0x00000000u})
        {
            page_t p(state);
            p.fill(0, 4, ERASED, 0x41);
            const size_t expected = state == 0xFFFFFFFEu || state == 0xFFFFFFFCu ? 1 : 0; // ACTIVE and FULL only
            CHECK(runs_of(p, runs) == expected);
        }
        page_t corrupt;
        corrupt.fill(0, 4, ERASED, 0x41);
        corrupt.b[28] ^= 0x80; // CRC of the header
        CHECK(runs_of(corrupt, runs) == 0);
    }

    void test_runs()
    {
        page_t p;
        p.fill(0, 3, WRITTEN, 0x11);   // written: never
        p.fill(3, 8, ERASED, 0x22);    // run 3..7
        p.fill(8, 10, ERASED, 0x00);   // purged: ends the run
        p.fill(10, 11, ERASED, 0x33);  // run 10
        p.fill(11, 12, WRITTEN, 0x00); // written (zero data is data): ends it
        p.fill(12, 14, ERASED, 0x44);  // run 12..13
        p.fill(14, 15, ILLEGAL, 0x55); // illegal: not erased, never (ESP-IDF erases it at the load)
        p.fill(15, 16, ERASED, 0x00);
        p.entry(15)[31] = 0x01;        // one byte left: data
        p.fill(16, 120, EMPTY, 0xff);
        p.fill(120, 126, ERASED, 0x66); // run to the last entry
        std::vector<nvs_purge_run_t> runs;
        CHECK(runs_of(p, runs) == 5);
        CHECK(run_is(runs[0], 3, 5));
        CHECK(run_is(runs[1], 10, 1));
        CHECK(run_is(runs[2], 12, 2));
        CHECK(run_is(runs[3], 15, 1));
        CHECK(run_is(runs[4], 120, 6));
    }

    void test_most_runs()
    {
        // Every second entry erased with data: 63 runs, the most a page can have
        page_t p;
        for (size_t i = 0; i < ENTRIES; ++i)
            p.fill(i, i + 1, i % 2 == 0 ? ERASED : WRITTEN, 0x77);
        std::vector<nvs_purge_run_t> runs;
        CHECK(runs_of(p, runs) == NVS_PURGE_MAX_RUNS);
        CHECK(run_is(runs[62], 124, 1));
        // All 126 erased with data: one run
        p.fill(0, ENTRIES, ERASED, 0x77);
        CHECK(runs_of(p, runs) == 1 && run_is(runs[0], 0, ENTRIES));
    }

    // ---- nvs_purge_start() against a fake partition ----

    struct fake_t
    {
        bool usable = true;
        std::vector<page_t> pages;
        std::vector<std::string> calls;
        std::set<size_t> read_error;  // pages
        std::set<size_t> write_error; // pages
        esp_partition_t partition{0xC40000, 0};
    };
    fake_t g;

    void reset(const std::vector<page_t> &pages)
    {
        g = fake_t{};
        g.pages = pages;
        g.partition.size = static_cast<uint32_t>(pages.size() * PAGE);
    }

    // Two pages as on a board: written items, erased entries with data and purged ones, an empty rest
    std::vector<page_t> board_like()
    {
        page_t a;
        a.fill(0, 10, WRITTEN, 0x51);
        a.fill(10, 20, ERASED, 0x52);
        a.fill(20, 30, ERASED, 0x00);
        a.fill(30, 126, WRITTEN, 0x53);
        page_t b(0xFFFFFFFEu, 8);
        b.fill(0, 5, ERASED, 0x54);
        b.fill(5, 7, WRITTEN, 0x55);
        b.fill(7, 8, ERASED, 0x56);
        page_t free_page;
        free_page.b.assign(PAGE, 0xff);
        return {a, b, free_page};
    }
} // namespace

bool nvs_state_usable()
{
    return g.usable;
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char *label)
{
    return g.partition.size > 0 && strcmp(label, "nvs") == 0 ? &g.partition : nullptr;
}

esp_err_t esp_partition_read(const esp_partition_t *, size_t src_offset, void *dst, size_t size)
{
    g.calls.push_back("read " + std::to_string(src_offset / PAGE));
    CHECK(src_offset % PAGE == 0 && size == PAGE && src_offset / PAGE < g.pages.size());
    if (g.read_error.count(src_offset / PAGE) != 0)
        return ESP_FAIL;
    memcpy(dst, g.pages[src_offset / PAGE].b.data(), PAGE);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *, size_t dst_offset, const void *src, size_t size)
{
    const size_t page = dst_offset / PAGE;
    const size_t at = dst_offset % PAGE;
    g.calls.push_back("write " + std::to_string(page) + " " + std::to_string((at - 64) / 32) + " " + std::to_string(size / 32));
    CHECK(page < g.pages.size() && at >= 64 && (at - 64) % 32 == 0 && size % 32 == 0 && at + size <= PAGE);
    if (g.write_error.count(page) != 0)
        return ESP_FAIL;
    const uint8_t *bytes = static_cast<const uint8_t *>(src);
    for (size_t i = 0; i < size; ++i)
    {
        CHECK(bytes[i] == 0);
        g.pages[page].b[at + i] &= bytes[i]; // NOR flash: a write only clears bits
    }
    return ESP_OK;
}

namespace
{
    void test_start_zeroes_erased_data_only()
    {
        reset(board_like());
        const std::vector<page_t> before = g.pages;
        nvs_purge_start();
        CHECK((g.calls == std::vector<std::string>{"read 0", "write 0 10 10", "read 0", "read 1", "write 1 0 5", "write 1 7 1", "read 1", "read 2"}));
        // Everything else byte for byte as before: headers, bitmaps, written and empty entries, the free page
        for (size_t p = 0; p < g.pages.size(); ++p)
            for (size_t i = 0; i < PAGE; ++i)
            {
                const bool zeroed = (p == 0 && i >= 64 + 10 * 32 && i < 64 + 20 * 32) || (p == 1 && i >= 64 && i < 64 + 5 * 32) ||
                                    (p == 1 && i >= 64 + 7 * 32 && i < 64 + 8 * 32);
                CHECK(g.pages[p].b[i] == (zeroed ? 0 : before[p].b[i]));
            }
        // The next start: read only
        g.calls.clear();
        nvs_purge_start();
        CHECK((g.calls == std::vector<std::string>{"read 0", "read 1", "read 2"}));
    }

    void test_errors_do_not_stop_the_other_pages()
    {
        reset(board_like());
        g.write_error = {0};
        g.read_error = {2};
        nvs_purge_start();
        std::vector<nvs_purge_run_t> runs;
        CHECK(runs_of(g.pages[0], runs) == 1); // left
        CHECK(runs_of(g.pages[1], runs) == 0); // zeroed all the same
    }

    void test_nothing_while_nvs_cannot_be_used()
    {
        reset(board_like());
        g.usable = false;
        nvs_purge_start();
        CHECK(g.calls.empty());
    }

    void test_no_partition()
    {
        reset({});
        nvs_purge_start();
        CHECK(g.calls.empty());
    }
} // namespace

void run_tests()
{
    test_real_page_header();
    test_page_states();
    test_runs();
    test_most_runs();
    test_start_zeroes_erased_data_only();
    test_errors_do_not_stop_the_other_pages();
    test_nothing_while_nvs_cannot_be_used();
    test_no_partition();
}

void fuzz_one(const uint8_t *, size_t)
{
}
