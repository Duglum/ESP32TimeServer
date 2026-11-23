// ESP32-WebTherm host tests: the state "NVS cannot be used" (main/sys/nvs_state_core.cpp): every error of
// nvs_flash_init() gets its cause, and every cause a text and a recovery.
#define TEST_NAME "nvs_state_test"
#include "check.h"

#include "nvs.h"
#include "sys/nvs_state.h"

namespace
{
    void test_classify()
    {
        CHECK(nvs_state_classify(ESP_OK) == nvs_state_cause_t::usable);
        CHECK(nvs_state_classify(ESP_ERR_NVS_NO_FREE_PAGES) == nvs_state_cause_t::full);
        CHECK(nvs_state_classify(ESP_ERR_NVS_NEW_VERSION_FOUND) == nvs_state_cause_t::newer_format);
        // No NVS partition: esp_partition_find_first() fails (ESP_ERR_NOT_FOUND), or the block device lookup
        CHECK(nvs_state_classify(ESP_ERR_NOT_FOUND) == nvs_state_cause_t::no_partition);
        CHECK(nvs_state_classify(ESP_ERR_NVS_PART_NOT_FOUND) == nvs_state_cause_t::no_partition);
        CHECK(nvs_state_classify(ESP_ERR_NO_MEM) == nvs_state_cause_t::no_memory);
        // Everything else cannot be used either: a read error of the flash, an entry table, encryption, an unknown code
        for (esp_err_t err : {ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT,
                              ESP_ERR_NVS_NOT_INITIALIZED, ESP_ERR_NVS_WRONG_ENCRYPTION, 0x6001, 0x7fffffff})
            CHECK(nvs_state_classify(err) == nvs_state_cause_t::other);
    }

    void test_texts()
    {
        CHECK_STR(nvs_state_cause_text(nvs_state_cause_t::usable), "");
        CHECK_STR(nvs_state_recovery_text(nvs_state_cause_t::usable), "");
        const nvs_state_cause_t causes[] = {nvs_state_cause_t::full, nvs_state_cause_t::newer_format, nvs_state_cause_t::no_partition,
                                            nvs_state_cause_t::no_memory, nvs_state_cause_t::other};
        for (size_t i = 0; i < sizeof(causes) / sizeof(causes[0]); ++i)
        {
            CHECK(nvs_state_cause_text(causes[i])[0] != '\0');
            CHECK(nvs_state_recovery_text(causes[i])[0] != '\0');
            for (size_t j = 0; j < i; ++j)
                CHECK(strcmp(nvs_state_cause_text(causes[i]), nvs_state_cause_text(causes[j])) != 0);
        }
        // The recovery of a missing partition is the table, of every other cause the erase of NVS over USB
        CHECK(strstr(nvs_state_recovery_text(nvs_state_cause_t::no_partition), "partition table") != nullptr);
        for (nvs_state_cause_t c : {nvs_state_cause_t::full, nvs_state_cause_t::newer_format, nvs_state_cause_t::no_memory, nvs_state_cause_t::other})
            CHECK(strstr(nvs_state_recovery_text(c), "erase NVS over the USB port") != nullptr);
        // A value outside the enum (a corrupted byte) still gets a text
        CHECK(nvs_state_cause_text(static_cast<nvs_state_cause_t>(200))[0] != '\0');
        CHECK(nvs_state_recovery_text(static_cast<nvs_state_cause_t>(200))[0] != '\0');
    }
} // namespace

void run_tests()
{
    test_classify();
    test_texts();
}

void fuzz_one(const uint8_t *, size_t)
{
}
