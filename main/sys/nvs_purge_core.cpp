// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The scan of an NVS page for erased entries that still hold data (see nvs_purge.h). Pure: host tested.

#include "sys/nvs_purge.h"

namespace
{
    // The page format of ESP-IDF 6.1.1 (nvs_page.hpp): a header of 32 bytes (state, sequence number, version, reserved,
    // CRC-32 of the bytes 4 to 27), the entry state bitmap of 32 bytes (2 bits per entry), then 126 entries of 32 bytes
    constexpr size_t ENTRY_COUNT = 126;
    constexpr size_t ENTRY_SIZE = 32;
    constexpr size_t BITMAP_OFFSET = 32;
    constexpr size_t ENTRIES_OFFSET = 64;
    constexpr uint32_t PAGE_ACTIVE = 0xFFFFFFFEu;
    constexpr uint32_t PAGE_FULL = 0xFFFFFFFCu;
    constexpr uint8_t ENTRY_ERASED = 0; // EntryState::ERASED (WRITTEN 2, EMPTY 3, ILLEGAL 1)
    // The format version in the header (nvs_constants.h: NVS_CONST_NVS_VERSION 0xfe, "Decrement to upgrade"). ESP-IDF
    // 6.1.1 uses it for one check only: a partition with a page of a smaller value is refused (Page::load():
    // ESP_ERR_NVS_NEW_VERSION_FOUND), so this scan never sees one there; a page with 0xff is read with the layout
    // above. A later ESP-IDF with a new format would accept its smaller value: such a page is not touched by code
    // written for the layout above.
    constexpr size_t VERSION_OFFSET = 8;
    constexpr uint8_t VERSION_KNOWN_MIN = 0xFE;

    uint32_t u32_at(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    }

    // esp_rom_crc32_le() (CRC-32, reflected, polynomial 0xEDB88320; the value is inverted on the way in and out)
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

    bool erased_with_data(const uint8_t *page, size_t index)
    {
        if (((page[BITMAP_OFFSET + index / 4] >> ((index % 4) * 2)) & 3u) != ENTRY_ERASED)
            return false;
        const uint8_t *entry = page + ENTRIES_OFFSET + index * ENTRY_SIZE;
        for (size_t i = 0; i < ENTRY_SIZE; ++i)
            if (entry[i] != 0)
                return true;
        return false;
    }
} // namespace

size_t nvs_purge_page_runs(const uint8_t *page, nvs_purge_run_t runs[NVS_PURGE_MAX_RUNS])
{
    // A page NVS uses: ACTIVE or FULL with the CRC of its header right (Page::load(), Header::calculateCrc32()).
    // nvs_flash_init() has resolved a FREEING page before; an uninitialised or corrupt page holds no entries for NVS.
    const uint32_t state = u32_at(page);
    if ((state != PAGE_ACTIVE && state != PAGE_FULL) || crc32_le(0xFFFFFFFFu, page + 4, 24) != u32_at(page + 28))
        return 0;
    if (page[VERSION_OFFSET] < VERSION_KNOWN_MIN)
        return 0; // a format newer than the one this scan was written for
    size_t count = 0;
    size_t index = 0;
    while (index < ENTRY_COUNT)
    {
        if (!erased_with_data(page, index))
        {
            ++index;
            continue;
        }
        size_t end = index + 1;
        while (end < ENTRY_COUNT && erased_with_data(page, end))
            ++end;
        runs[count].offset = static_cast<uint16_t>(ENTRIES_OFFSET + index * ENTRY_SIZE);
        runs[count].size = static_cast<uint16_t>((end - index) * ENTRY_SIZE);
        ++count; // at most 63: each run but the last is followed by an entry that ends it
        index = end + 1;
    }
    return count;
}
