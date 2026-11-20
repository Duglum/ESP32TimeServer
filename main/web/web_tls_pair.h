// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// The own web certificate in NVS: chain and private key as ONE blob, "<chain PEM>\0<key PEM>\0". NVS of ESP-IDF 6.1.1
// changes one blob all or nothing (the new version is written in full, the old one is erased only after it; at the
// start a new version without its index is removed and of two complete versions the newer one is kept), so after a
// failed store or a power loss the next start finds the old pair or the new pair, never a chain with another key.
// No ESP-IDF dependencies: also built by the host tests (testing/host/web_tls_pair_test.cpp).
//
// Three layouts exist, and a board can run a firmware of each of them again after an update (namespace "webtls"):
// - before 478c2ff (2026-11-19): chain and key as two blobs "ocrt" and "okey", written one after the other;
// - 478c2ff to d77f972: the one blob "opair"; at its start "opair" wins and "ocrt"/"okey" are erased;
// - since then: the one blob "opair2" and a marker in "okey" (WEB_TLS_OWN_MARKER, without a NUL at the end, so the older
//   firmwares read it as "no key"). An older firmware erases the marker at Reset to defaults and when it stores a pair,
//   but never writes it: "opair2" counts only while the marker is there. The marker is in "okey" and not in "ocrt":
//   the firmware before 478c2ff writes "ocrt" first and "okey" second, so a store of it that was cut off or failed after
//   the first write leaves the marker, and a store of it that went through leaves a complete "ocrt"/"okey" pair.
// "mode" (1: the own certificate is in use) is shared by all of them and written after the pair by each of them; only
// a Reset to defaults erases it.
// The rule below decides from which items exist which pair is the one stored last, and lists the NVS steps of a start,
// a store and a clear in an order in which a cut-off after any step leaves the old or the new pair.

#pragma once

#include <cstddef>
#include <cstdint>

// Bytes of the blob for a chain and a key of these lengths (characters, without their NUL)
constexpr size_t web_tls_pair_size(size_t cert_length, size_t key_length)
{
    return cert_length + 1 + key_length + 1;
}

// Writes "<cert>\0<key>\0" to out. Returns the bytes written, 0 when out is too small or a text is empty.
size_t web_tls_pair_pack(const char *cert, const char *key, uint8_t *out, size_t out_size);

// Splits a stored blob: cert and key point into data (each terminated by its NUL). False when the blob is not two
// non-empty texts that end with the last byte of the blob.
bool web_tls_pair_split(const uint8_t *data, size_t size, const char *&cert, const char *&key);

// NVS entries (32 bytes) a blob of size bytes takes at most when it is written: the data (each chunk rounded up to a
// whole entry), one header per chunk and the index. A chunk holds at most 4000 bytes (one page of 126 entries less its
// header); the first chunk fills the rest of the page in use, so there is at most one chunk more than size / 4000.
size_t web_tls_nvs_entries_max(size_t size);

// NVS entries a stored blob of size bytes takes at least: its data, one header and the index
size_t web_tls_nvs_entries_min(size_t size);

// Room check before a blob of new_size bytes (and extra_entries for other items written with it) is stored next to the
// old data (old_entries_min: what is erased once the new blob is stored). available: nvs_stats_t.available_entries.
// reserve: the entries NVS keeps free afterwards (for writing the settings again). A store that does not grow the data
// is only refused when the new data does not fit next to the old data. needed: the entries checked against available.
bool web_tls_nvs_room(size_t available, size_t new_size, size_t extra_entries, size_t old_entries_min, size_t reserve, size_t &needed);

// ---- Which pair is stored, and the NVS steps ----

// The marker in "okey" (no NUL: the firmwares before read_pem() it as "no key")
constexpr char WEB_TLS_OWN_MARKER[] = {'o', 'p', 'a', 'i', 'r', '2'};
constexpr size_t WEB_TLS_OWN_MARKER_SIZE = sizeof(WEB_TLS_OWN_MARKER);

enum class web_tls_okey_t : uint8_t
{
    absent,
    marker, // exactly WEB_TLS_OWN_MARKER
    key     // any other blob (written by the firmware before 478c2ff)
};

// ocrt_in / okey_in: the whole items whose chain / key is byte for byte the text in "ocrt" / "okey"
constexpr uint8_t WEB_TLS_OWN_IN_OPAIR = 1;
constexpr uint8_t WEB_TLS_OWN_IN_OPAIR2 = 2;

// Which items exist, the class of "okey" and which halves are equal (nothing about whether a pair passes the check)
struct web_tls_own_items_t
{
    bool ocrt = false;
    web_tls_okey_t okey = web_tls_okey_t::absent;
    bool opair = false;
    bool opair2 = false;
    bool mode = false;
    uint8_t ocrt_in = 0; // WEB_TLS_OWN_IN_*: "ocrt" is the chain of that item
    uint8_t okey_in = 0; // WEB_TLS_OWN_IN_*: "okey" (a key) is the key of that item
    // Store only: the new pair's chain is the text in "ocrt" (bit 0), its key the one in "okey" (bit 1)
    uint8_t new_in = 0;
};

enum class web_tls_own_source_t : uint8_t
{
    none,   // no own pair stored
    former, // "ocrt" and "okey" (used when they pass the check, else none)
    opair,  // "opair"
    opair2  // "opair2"
};

enum class web_tls_own_step_t : uint8_t
{
    erase_ocrt,
    erase_okey,
    erase_opair,
    erase_opair2,
    write_marker,     // "okey" = WEB_TLS_OWN_MARKER
    copy_opair,       // "opair2" = the blob of "opair"
    write_new         // "opair2" = the new pair (store only)
};

struct web_tls_own_plan_t
{
    web_tls_own_source_t source = web_tls_own_source_t::none; // start: the pair to use; store, clear: the one before
    uint8_t count = 0;
    web_tls_own_step_t steps[7]{};
    // store: the index of the step with which the new pair becomes the stored one (the steps before it leave the old
    // pair, the steps from it on the new one); count when the plan has none
    uint8_t commit = 0;
};

// The rule (o = "okey"):
//   o is a key:   "ocrt" exists -> former (the firmware before 478c2ff stored it last); else none (its reset or the
//                 erase of a later firmware was cut off between "ocrt" and "okey")
//   o is marker:  "opair" exists -> opair (478c2ff to d77f972 stored it after this firmware wrote the marker, or the
//                 move into "opair2" was cut off); else "opair2" exists -> opair2; else none
//   o is absent:  "opair" and "mode" exist -> opair (a board of 478c2ff to d77f972); else none ("opair2" without the
//                 marker: an older firmware did a Reset to defaults; "opair" without "mode": the firmware before
//                 478c2ff did a Reset to defaults, or the store that wrote it failed before its "mode")
// and before that, while "okey" is not the marker: when "ocrt" (first) or the key in "okey" is a half of a pair that
// is whole in "opair2" (first) or "opair", that item; when both are the halves of the same whole item, former. The
// firmware before 478c2ff stored that pair again (after a downgrade the page shows no own certificate) and that store,
// a later one, a reset or an erase of 478c2ff was cut off or failed halfway.
web_tls_own_source_t web_tls_own_decide(const web_tls_own_items_t &items);

// The steps at the start: the items that lost are erased, a pair in "opair" is moved into "opair2"
web_tls_own_plan_t web_tls_own_start(const web_tls_own_items_t &items);

// The steps of storing a new pair (the caller writes "mode" after them): the steps of a start, then the new pair into
// "opair2", the marker, "ocrt" erased.
web_tls_own_plan_t web_tls_own_store(const web_tls_own_items_t &items);

// The steps of Reset to defaults (the caller erases "mode" after them)
web_tls_own_plan_t web_tls_own_clear(const web_tls_own_items_t &items);

// What a step does to the items (for the commit index and the host tests)
void web_tls_own_apply(web_tls_own_items_t &items, web_tls_own_step_t step);
