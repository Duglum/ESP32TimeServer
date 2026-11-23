// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Presence marks of stored security records: a lost record does not fall back to a default.
//
// A record (a blob in NVS: a password hash, an allow-list, a token, a settings set) that is missing at a start reads the
// same as one that was never stored, and the firmware would go back to its default for it (the compiled password, no
// allow-list, no second factor). Each such record gets a mark: a NUMBER item (u64) in its own namespace
// RECORD_MARK_NVS_NAMESPACE, key "<namespace>/<key>" of the record. A number item is one entry, written in one flash
// write; the start-up check of ESP-IDF's NVS treats number items correctly.
//
// Write order, so that "a mark without a valid record" can only come from a loss of the record:
// - store or change: the record first (nvs_set_blob, nvs_commit), then record_mark_set();
// - erase: record_mark_clear() first, then the record; a reset of every record: record_mark_clear_all() first;
// - at every start: record_mark_check() for each record; a valid record without a mark gets one (boards that stored
//   their records before the marks existed, and a reset between the record and its mark).
// A cut-off at any point leaves the record without a mark or with its mark, never a mark alone. What the marks cannot
// see: a record lost while it had no mark yet, and an erase of the whole NVS (both read as "never stored").
//
// Project-independent: the same files in every project that uses them. The pure part (record_mark_decide(),
// record_mark_key(), record_mark_text_hash()) is in record_mark_core.cpp, host tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

constexpr char RECORD_MARK_NVS_NAMESPACE[] = "recmark";
constexpr size_t RECORD_MARK_KEY_SIZE = 16; // NVS_KEY_NAME_MAX_SIZE: 15 characters and the NUL

enum class record_mark_state_t : uint8_t
{
    none,    // no mark, no valid record: never stored (or erased)
    present, // mark and valid record
    adopted, // a valid record without a mark: the mark was set now
    lost,    // a mark without a valid record: the record was lost
};

// The state of a record from its mark and its record (pure)
record_mark_state_t record_mark_decide(bool marked, bool record_valid);
// The key of the mark of record <ns>/<key>: "<ns>/<key>". false (out empty) when it has more than 15 characters.
bool record_mark_key(const char *ns, const char *key, char (&out)[RECORD_MARK_KEY_SIZE]);
// 64-bit FNV-1a of a text: a mark value that tells which record it belongs to without storing the text (for example
// the user of a token). Not a secret; never 0.
uint64_t record_mark_text_hash(const char *text);

// At a start, for record <ns>/<key>: reads the mark and decides (record_mark_decide). present with another value and
// adopted: the mark is written with value. *stored (may be nullptr): the value of the mark that was there (0 without
// one), for a lost record the only thing left of it. A read error of the mark counts as a mark (fails closed) and is
// logged. NVS that is not initialised counts as no mark, not logged: that is the state of nvs_state.h, in which the
// firmware refuses every login without looking at any record (as a mark it would read every record as lost).
record_mark_state_t record_mark_check(const char *ns, const char *key, bool record_valid, uint64_t value, uint64_t *stored);
// After the record was stored and committed: the mark (written only when it is missing or has another value)
esp_err_t record_mark_set(const char *ns, const char *key, uint64_t value);
// Before the record is erased. ESP_OK also when there was no mark.
esp_err_t record_mark_clear(const char *ns, const char *key);
// Before every record is erased (Reset to defaults): all marks. ESP_OK also when there were none.
esp_err_t record_mark_clear_all();
