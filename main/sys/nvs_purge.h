// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Replaced and erased NVS data is purged from the flash.
//
// ESP-IDF's NVS (6.1.1) writes the new value of a key before it erases the old one, and an erase only marks the item's
// entries as erased in the state bitmap of their page. With a handle opened NVS_READWRITE the old bytes stay in the
// flash until a garbage collection erases that page, which can take a long time: a replaced or erased private key,
// password record, TOTP secret or settings set with its passwords (also after Reset to defaults) could be read from
// the flash over the USB port. A handle opened NVS_READWRITE_PURGE writes zeros over every entry of each item it erases
// or replaces, right after the state bits (nvs_page.cpp: Page::eraseEntryAndSpan(), purgeEntryRange()). So every
// writer of the firmware opens NVS with NVS_READWRITE_PURGE.
//
// That leaves the entries an older firmware erased without zeros, and an erase cut off by a reset or a power loss
// between its state bits and the last of its zeros. ESP-IDF zeroes an item from its first entry (the item header) to
// its last, and its own purge of erased items (nvs_purge_all()) only finds an item whose header is still intact: a
// cut-off in the middle leaves the rest of the item, up to a chunk of 4000 bytes (measured on the test board: up to
// 121 entries of an own certificate and key per cut-off). nvs_purge_all() also reads every erased entry on its own
// (2.3 s per start for the six namespaces of the test board). So at every start nvs_purge_start() reads each page of
// the partition once and writes zeros over every entry that the state bitmap marks erased and that still holds data,
// whatever it belonged to. ESP-IDF never reads the bytes of an erased entry, apart from that header check, which an
// entry of zeros fails: it skips erased entries when it searches, copies (garbage collection) and loads a page, and an
// erased entry is never written again before its page is erased. Writing zeros only clears bits, as NVS's own purge
// does.
//
// Not covered: copies outside the NVS partition (see the README); an encrypted NVS partition (the build stops).
//
// Project-independent: the same files in every project that uses them. The scan of a page (nvs_purge_core.cpp) is
// pure and host tested; it follows the page format of ESP-IDF 6.1.1 (nvs_page.hpp, nvs_types.hpp).
#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t NVS_PURGE_PAGE_SIZE = 4096; // Page::SEC_SIZE
constexpr size_t NVS_PURGE_MAX_RUNS = 63;    // 126 entries, a run is followed by at least one other entry

// Consecutive entries to be zeroed: byte offset in the page and number of bytes (whole entries of 32 bytes)
struct nvs_purge_run_t
{
    uint16_t offset;
    uint16_t size;
};

// The runs of consecutive entries of one page (NVS_PURGE_PAGE_SIZE bytes) that the state bitmap marks erased and that
// still hold data (not all zero), at most NVS_PURGE_MAX_RUNS; 0 for a page NVS does not use (only ACTIVE and FULL pages
// with a right header CRC are looked at). Pure.
size_t nvs_purge_page_runs(const uint8_t *page, nvs_purge_run_t runs[NVS_PURGE_MAX_RUNS]);

// At the start, after nvs_state_init() and before anything else writes NVS: zeroes every erased entry of the NVS
// partition that still holds data, then reads those pages again. One I line with the number of entries and the time,
// a W line for a page that could not be read or written or still holds such data. Nothing while NVS cannot be used
// (nvs_state.h).
void nvs_purge_start();
