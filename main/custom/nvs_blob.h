// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Writes an NVS blob only when NVS does not already hold exactly these bytes under its key.
//
// Workaround for a fault of ESP-IDF 6.1.1 (components/nvs_flash/src/nvs_storage.cpp, Storage::cmpMultiPageBlob(),
// line 758): the CRC of every chunk is computed from the start of the caller's buffer instead of the chunk's own offset
// (the cmpItem() call at line 762 has "+ offset"). Only the first chunk can match, so nvs_set_blob() takes an unchanged
// blob of more than one chunk as changed (Storage::writeItem(), line 490): it writes the whole blob again as a new
// version and erases the old one afterwards, and it needs room for the second copy while doing so. Upstream master has
// "+ offset", release/v6.1 had not (checked 2026-11-19). Remove this helper (back to nvs_set_blob()) once the ESP-IDF
// in use has the correction.
//
// Only blobs of more than NVS_BLOB_ONE_CHUNK_MAX bytes can have more than one chunk (Storage::writeMultiPageBlob(),
// line 295: a blob that does not fit the rest of the current page goes to a new page while that rest is below
// Page::CHUNK_MAX_SIZE / 10 = 400 bytes; a chunk holds at most 4000 bytes). Smaller blobs need no workaround.
#pragma once

#include <cstddef>

#include "esp_err.h"
#include "nvs.h"

static constexpr size_t NVS_BLOB_ONE_CHUNK_MAX = 400;

// As nvs_set_blob(handle, key, data, size), but reads the stored blob first (into a heap buffer that is zeroed before
// it is freed) and writes nothing when it has the same size and bytes; then ESP_OK and *written false. Anything else
// (no stored blob, other size or bytes, read error, no memory for the copy) goes to nvs_set_blob() as before, *written
// true if that returned ESP_OK. No nvs_commit(): the caller commits as before. written may be nullptr.
esp_err_t nvs_blob_set_if_changed(nvs_handle_t handle, const char *key, const void *data, size_t size, bool *written);
