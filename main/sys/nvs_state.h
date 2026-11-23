// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// NVS that cannot be used: never erased by the firmware, and a state the firmware asks for.
//
// nvs_flash_init() fails when the partition has no free page (ESP_ERR_NVS_NO_FREE_PAGES: full, or written for another
// size), when a page is of a newer format (ESP_ERR_NVS_NEW_VERSION_FOUND), when the partition table has no NVS
// partition (ESP_ERR_NOT_FOUND), when there is not enough memory, or on a read error of the flash. ESP-IDF's examples
// erase the whole partition on the first two and start again: every stored record would be gone, the presence marks
// (record_mark.h) too, and the device would start on its defaults (the compiled password) without a trace. Here nothing
// is erased: the state is kept for this start, the firmware refuses every login while it lasts and stores nothing, and
// the recovery is the erase of NVS over the USB port (a person at the device decides that the stored data may go).
//
// The state lives in RAM only and is decided again at every start: a passing error (no memory) is over at the next
// start that does not have it. A partition that was never written (all FF) is no error: a new device starts normally.
//
// Project-independent: the same files in every project that uses them. The pure part (nvs_state_classify() and the
// texts) is in nvs_state_core.cpp, host tested.
#pragma once

#include <cstdint>

#include "esp_err.h"

enum class nvs_state_cause_t : uint8_t
{
    usable,       // ESP_OK
    full,         // ESP_ERR_NVS_NO_FREE_PAGES: no free page (full, or written for another partition size)
    newer_format, // ESP_ERR_NVS_NEW_VERSION_FOUND: a page of a newer NVS format (a newer ESP-IDF wrote it)
    no_partition, // ESP_ERR_NOT_FOUND, ESP_ERR_NVS_PART_NOT_FOUND: the partition table has no NVS partition
    no_memory,    // ESP_ERR_NO_MEM: passing; the next start may work
    other,        // anything else (a read error of the flash, an entry table that cannot be read, encryption)
};

// The cause of an error of nvs_flash_init() (pure)
nvs_state_cause_t nvs_state_classify(esp_err_t init_error);
// What the cause means, for the console (pure; "" for usable)
const char *nvs_state_cause_text(nvs_state_cause_t cause);
// What a person at the device does about it, for the console (pure; "" for usable). The erase command itself is
// added by nvs_state_init() with the offset and size of the partition.
const char *nvs_state_recovery_text(nvs_state_cause_t cause);

// At the start, before anything opens NVS: nvs_flash_init(), never an erase. On an error one E line with the error,
// its cause and the recovery. Returns the error of nvs_flash_init().
esp_err_t nvs_state_init();
// NVS was initialised at this start (false before nvs_state_init())
bool nvs_state_usable();
// The error of nvs_flash_init() at this start (ESP_OK when usable, ESP_ERR_NVS_NOT_INITIALIZED before nvs_state_init())
esp_err_t nvs_state_error();
