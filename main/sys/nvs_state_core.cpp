// ESP32-WebTherm
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// The pure part of the state "NVS cannot be used" (see nvs_state.h). Host tested.

#include "sys/nvs_state.h"

#include "nvs.h"

nvs_state_cause_t nvs_state_classify(esp_err_t init_error)
{
    switch (init_error)
    {
    case ESP_OK:
        return nvs_state_cause_t::usable;
    case ESP_ERR_NVS_NO_FREE_PAGES:
        return nvs_state_cause_t::full;
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return nvs_state_cause_t::newer_format;
    case ESP_ERR_NOT_FOUND:          // esp_partition_find_first() found no NVS partition (nvs_partition_lookup.cpp)
    case ESP_ERR_NVS_PART_NOT_FOUND: // the same with CONFIG_NVS_BDL_STACK
        return nvs_state_cause_t::no_partition;
    case ESP_ERR_NO_MEM:
        return nvs_state_cause_t::no_memory;
    default:
        return nvs_state_cause_t::other;
    }
}

const char *nvs_state_cause_text(nvs_state_cause_t cause)
{
    switch (cause)
    {
    case nvs_state_cause_t::usable:
        return "";
    case nvs_state_cause_t::full:
        return "no free page: the partition is full or was written for another size";
    case nvs_state_cause_t::newer_format:
        return "a page is of a newer NVS format (written by a newer firmware)";
    case nvs_state_cause_t::no_partition:
        return "the partition table has no NVS partition";
    case nvs_state_cause_t::no_memory:
        return "not enough memory to load it";
    case nvs_state_cause_t::other:
        break;
    }
    return "it could not be read";
}

const char *nvs_state_recovery_text(nvs_state_cause_t cause)
{
    switch (cause)
    {
    case nvs_state_cause_t::usable:
        return "";
    case nvs_state_cause_t::no_partition:
        return "write the partition table of this firmware over the USB port";
    case nvs_state_cause_t::no_memory:
        return "restart the device; if this stays, erase NVS over the USB port (every stored setting and key is lost)";
    default:
        break;
    }
    return "erase NVS over the USB port (every stored setting and key is lost)";
}
