// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "gnss_nmea.h"
#include "gnss_uart.h"
#include "ESP32TimeServerSettings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{
#if DEBUG_ENABLED
    static const char *TAG = "gnss_nmea";
#endif

    static constexpr int64_t GGA_MAX_AGE_US = 2000000;   // an older GGA does not count
    // The RMC phase is only taken within 1 s after the last PPS edge: a later RMC belongs to a second whose edge was
    // missing (PPS lost, or ignored while the GNSS time is not valid) and would count against the old edge
    static constexpr int64_t RMC_PHASE_WINDOW_US = 1000000;
    static constexpr int64_t GSV_GROUP_EXPIRY_US = 10000000;
    static constexpr size_t GSV_GROUP_LIMIT = 8;

    // Latest RMC (valid or not), written by the reader task, read by the waiting tasks (s_lock)
    struct rmc_result_t
    {
        uint32_t seq;        // counts the RMC sentences with a correct checksum
        uint32_t generation; // flush generation at the start of the sentence
        bool valid;
        int64_t rx_us;       // esp_timer time when the end of the sentence was processed
        nmea_rmc_time_t time;
    };

    struct gsv_group_t
    {
        char talker[3];
        char signal_id[4];
        uint8_t message_count;
        uint8_t next_message_number;
        uint8_t satellites_in_view;
        int64_t last_updated_us;
        bool published;
    };

    portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
    rmc_result_t s_rmc{};
    SemaphoreHandle_t s_rmc_signal = nullptr; // given for every RMC

    gnss_nmea_config_t s_config{};

    std::atomic<uint8_t> s_min_satellites{0};
    std::atomic<int64_t> s_gga_rx_us{0};
    std::atomic<uint8_t> s_gga_fix_quality{0};
    std::atomic<uint8_t> s_gga_satellites{0};
    std::atomic<int32_t> s_rmc_phase_ms{-1};
    std::atomic<int32_t> s_rmc_phase_min_ms{-1};
    std::atomic<int32_t> s_rmc_phase_max_ms{-1};
    std::atomic<uint32_t> s_sentences{0};
    std::atomic<uint32_t> s_checksum_errors{0};
    std::atomic<uint32_t> s_rmc_phase_bins[GNSS_NMEA_RMC_PHASE_BINS] = {}; // RMC receptions per 100 ms after the PPS

    gsv_group_t s_gsv_groups[GSV_GROUP_LIMIT]{}; // only used by the reader task

    bool is_digit_char(char c)
    {
        return c >= '0' && c <= '9';
    }

    bool parse_two_digits(const char *text, int &value)
    {
        if (text == nullptr || !is_digit_char(text[0]) || !is_digit_char(text[1]))
            return false;
        value = (text[0] - '0') * 10 + (text[1] - '0');
        return true;
    }

    bool get_nmea_field(const char *sentence, int field_index, char *output, size_t output_size)
    {
        if (sentence == nullptr || output == nullptr || output_size == 0 || field_index < 0)
            return false;

        const char *cursor = sentence;
        if (*cursor == '$')
            cursor++;

        int current_field = 0;
        const char *field_start = cursor;

        while (true)
        {
            const char current = *cursor;
            const bool is_delimiter = (current == ',') || (current == '*') || (current == '\0') || (current == '\r') || (current == '\n');

            if (is_delimiter)
            {
                if (current_field == field_index)
                {
                    size_t length = static_cast<size_t>(cursor - field_start);
                    if (length >= output_size)
                        length = output_size - 1;

                    memcpy(output, field_start, length);
                    output[length] = '\0';
                    return true;
                }

                if (current != ',')
                    break;

                current_field++;
                cursor++;
                field_start = cursor;
                continue;
            }

            cursor++;
        }

        output[0] = '\0';
        return false;
    }

    // Unsigned decimal field of at most 3 digits (no sscanf: small task stacks)
    bool parse_nmea_uint8_field(const char *sentence, int field_index, uint8_t *value)
    {
        if (value == nullptr)
            return false;

        char field[4] = "";
        if (!get_nmea_field(sentence, field_index, field, sizeof(field)) || field[0] == '\0')
            return false;

        unsigned int parsed_value = 0;
        for (const char *cursor = field; *cursor != '\0'; ++cursor)
        {
            if (!is_digit_char(*cursor))
                return false;
            parsed_value = parsed_value * 10 + static_cast<unsigned int>(*cursor - '0');
        }
        if (parsed_value > UINT8_MAX)
            return false;

        *value = static_cast<uint8_t>(parsed_value);
        return true;
    }

    // Sentence type (e.g. "RMC") of "$ttRMC,..."
    bool sentence_is(const char *sentence, const char *type)
    {
        char sentence_type[16] = "";
        if (!get_nmea_field(sentence, 0, sentence_type, sizeof(sentence_type)))
            return false;
        const size_t length = strlen(sentence_type);
        return length == 5 && strcmp(sentence_type + 2, type) == 0;
    }

    bool gga_recent(int64_t now_us)
    {
        const int64_t rx_us = s_gga_rx_us.load(std::memory_order_relaxed);
        return rx_us != 0 && now_us - rx_us <= GGA_MAX_AGE_US;
    }

    // GGA fix quality: 1 GPS, 2 differential, 3 PPS, 4 RTK fixed, 5 RTK float. Not usable: 0 no fix, 6 estimated
    // (dead reckoning), 7 manual input, 8 simulator.
    bool gga_fix_quality_ok(uint8_t quality)
    {
        return quality >= 1 && quality <= 5;
    }

    void report_satellites(uint8_t count)
    {
        if (s_config.on_satellites != nullptr)
            s_config.on_satellites(count);
    }

    // ---- GSV: satellites in view (only reported while there is no GGA) ----
    void publish_gsv_satellite_count(int64_t now_us)
    {
        if (gga_recent(now_us))
            return;

        bool has_constellation_specific_group = false;
        bool has_reportable_group = false;
        for (const gsv_group_t &group : s_gsv_groups)
        {
            if (!group.published || (now_us - group.last_updated_us) > GSV_GROUP_EXPIRY_US)
                continue;

            has_reportable_group = true;
            if (strcmp(group.talker, "GN") != 0)
            {
                has_constellation_specific_group = true;
                break;
            }
        }

        if (!has_reportable_group)
            return;

        uint16_t total_satellites = 0;
        for (const gsv_group_t &group : s_gsv_groups)
        {
            if (group.published && (now_us - group.last_updated_us) <= GSV_GROUP_EXPIRY_US &&
                (!has_constellation_specific_group || strcmp(group.talker, "GN") != 0))
                total_satellites += group.satellites_in_view;
        }
        report_satellites(static_cast<uint8_t>(std::min<uint16_t>(total_satellites, UINT8_MAX)));
    }

    void expire_gsv_groups(int64_t now_us)
    {
        bool group_expired = false;
        for (gsv_group_t &group : s_gsv_groups)
        {
            if (group.talker[0] != '\0' && (now_us - group.last_updated_us) > GSV_GROUP_EXPIRY_US)
            {
                group = gsv_group_t{};
                group_expired = true;
            }
        }

        if (group_expired)
            publish_gsv_satellite_count(now_us);
    }

    void process_gsv(const char *sentence, int64_t now_us)
    {
        uint8_t message_count = 0;
        uint8_t message_number = 0;
        uint8_t satellites_in_view = 0;
        if (!parse_nmea_uint8_field(sentence, 1, &message_count) ||
            !parse_nmea_uint8_field(sentence, 2, &message_number) ||
            !parse_nmea_uint8_field(sentence, 3, &satellites_in_view) ||
            message_count == 0 || message_number == 0 || message_number > message_count)
            return;

        const uint16_t satellite_offset = static_cast<uint16_t>(message_number - 1) * 4U;
        const uint8_t satellites_in_message = satellites_in_view > satellite_offset
                                                  ? std::min<uint8_t>(4, static_cast<uint8_t>(satellites_in_view - satellite_offset))
                                                  : 0;
        char signal_id[4] = "";
        (void)get_nmea_field(sentence, 4 + satellites_in_message * 4, signal_id, sizeof(signal_id));

        expire_gsv_groups(now_us);

        gsv_group_t *group = nullptr;
        gsv_group_t *available_group = nullptr;
        for (gsv_group_t &candidate : s_gsv_groups)
        {
            if (candidate.talker[0] == '\0')
            {
                available_group = &candidate;
                continue;
            }
            if (strncmp(candidate.talker, sentence + 1, 2) == 0 && strcmp(candidate.signal_id, signal_id) == 0)
            {
                group = &candidate;
                break;
            }
        }

        if (group == nullptr && available_group != nullptr)
        {
            group = available_group;
            snprintf(group->talker, sizeof(group->talker), "%.2s", sentence + 1);
            snprintf(group->signal_id, sizeof(group->signal_id), "%s", signal_id);
        }

        if (group == nullptr || (message_number != 1 && (group->message_count != message_count || group->next_message_number != message_number)))
            return;

        group->message_count = message_count;
        group->next_message_number = static_cast<uint8_t>(message_number + 1);
        group->satellites_in_view = satellites_in_view;
        group->last_updated_us = now_us;
        group->published = message_number == message_count;

        if (group->published)
            publish_gsv_satellite_count(now_us);
    }

    // ---- GGA: fix quality (field 6) and satellites in use (field 7) ----
    void process_gga(const char *sentence, int64_t now_us)
    {
        uint8_t quality = 0;
        uint8_t satellites = 0;
        if (!parse_nmea_uint8_field(sentence, 6, &quality))
            return;
        if (!parse_nmea_uint8_field(sentence, 7, &satellites))
            satellites = 0;
        s_gga_fix_quality.store(quality, std::memory_order_relaxed);
        s_gga_satellites.store(satellites, std::memory_order_relaxed);
        s_gga_rx_us.store(now_us, std::memory_order_relaxed);
        report_satellites(satellites);
    }

    // ---- RMC: the time label; its reception phase after the PPS edge is kept for the diagnosis ----
    void process_rmc(const char *sentence, uint32_t generation, int64_t now_us)
    {
        const int64_t last_pps_us = s_config.last_pps_us != nullptr ? s_config.last_pps_us() : 0;
        if (last_pps_us > 0 && now_us >= last_pps_us && now_us - last_pps_us < RMC_PHASE_WINDOW_US)
        {
            const int32_t phase_ms = static_cast<int32_t>((now_us - last_pps_us) / 1000);
            s_rmc_phase_ms.store(phase_ms, std::memory_order_relaxed);
            const int32_t min_ms = s_rmc_phase_min_ms.load(std::memory_order_relaxed);
            if (min_ms < 0 || phase_ms < min_ms)
                s_rmc_phase_min_ms.store(phase_ms, std::memory_order_relaxed);
            if (phase_ms > s_rmc_phase_max_ms.load(std::memory_order_relaxed))
                s_rmc_phase_max_ms.store(phase_ms, std::memory_order_relaxed);
            s_rmc_phase_bins[std::min<int32_t>(phase_ms / 100, GNSS_NMEA_RMC_PHASE_BINS - 1)].fetch_add(1, std::memory_order_relaxed);
        }

        nmea_rmc_time_t time{};
        bool valid = gnss_nmea_parse_rmc(sentence, &time);
        // Optional GGA: while it arrives, its fix quality has to agree. With a minimum number of satellites GGA is
        // required and has to report at least that many.
        const bool gga = gga_recent(now_us);
        if (valid && gga && !gga_fix_quality_ok(s_gga_fix_quality.load(std::memory_order_relaxed)))
            valid = false;
        const uint8_t min_satellites = s_min_satellites.load(std::memory_order_relaxed);
        if (valid && min_satellites != 0 && (!gga || s_gga_satellites.load(std::memory_order_relaxed) < min_satellites))
            valid = false;

        taskENTER_CRITICAL(&s_lock);
        s_rmc.seq++;
        s_rmc.generation = generation;
        s_rmc.valid = valid;
        s_rmc.rx_us = now_us;
        s_rmc.time = time;
        taskEXIT_CRITICAL(&s_lock);
        if (s_rmc_signal != nullptr)
            xSemaphoreGive(s_rmc_signal);
    }
}

bool gnss_nmea_checksum_valid(const char *sentence)
{
    if (sentence == nullptr || sentence[0] != '$')
        return false;

    const char *checksum_marker = strrchr(sentence, '*');
    if (checksum_marker == nullptr || checksum_marker[1] == '\0' || checksum_marker[2] == '\0' || checksum_marker[3] != '\0')
        return false;

    // Two hexadecimal digits (no sscanf: this also runs in tasks with a small stack)
    const auto hex_value = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    const int high = hex_value(checksum_marker[1]);
    const int low = hex_value(checksum_marker[2]);
    if (high < 0 || low < 0)
        return false;
    const unsigned int expected_checksum = static_cast<unsigned int>(high * 16 + low);

    uint8_t actual_checksum = 0;
    for (const char *cursor = sentence + 1; cursor < checksum_marker; ++cursor)
        actual_checksum ^= static_cast<uint8_t>(*cursor);

    return actual_checksum == expected_checksum;
}

bool gnss_nmea_parse_rmc(const char *sentence, nmea_rmc_time_t *time_data)
{
    if (sentence == nullptr || time_data == nullptr)
        return false;

    char sentence_type[16] = "";
    if (!get_nmea_field(sentence, 0, sentence_type, sizeof(sentence_type)))
        return false;

    const size_t sentence_type_len = strlen(sentence_type);
    if (sentence_type_len < 3 || strcmp(sentence_type + sentence_type_len - 3, "RMC") != 0)
        return false;

    char status_field[4] = "";
    if (!get_nmea_field(sentence, 2, status_field, sizeof(status_field)))
        return false;

    if (status_field[0] != 'A')
        return false;

    // Mode indicator (field 12, NMEA 2.3 and later): only an autonomous, differential, precise or RTK solution counts.
    // E (dead reckoning), N (not valid), M (manual input), S (simulator) and unknown letters are refused; a receiver
    // that sends no mode indicator is judged by the status alone, as before.
    char mode_field[4] = "";
    if (get_nmea_field(sentence, 12, mode_field, sizeof(mode_field)) && mode_field[0] != '\0' &&
        strchr("ADPRF", mode_field[0]) == nullptr)
        return false;

    char time_field[16] = "";
    char date_field[16] = "";
    if (!get_nmea_field(sentence, 1, time_field, sizeof(time_field)) || !get_nmea_field(sentence, 9, date_field, sizeof(date_field)))
        return false;

    if (strlen(time_field) < 6 || strlen(date_field) < 6)
        return false;

    int hour = 0;
    int minute = 0;
    int second = 0;
    int day = 0;
    int month = 0;
    int year_two_digit = 0;

    if (!parse_two_digits(time_field + 0, hour) ||
        !parse_two_digits(time_field + 2, minute) ||
        !parse_two_digits(time_field + 4, second) ||
        !parse_two_digits(date_field + 0, day) ||
        !parse_two_digits(date_field + 2, month) ||
        !parse_two_digits(date_field + 4, year_two_digit))
    {
        return false;
    }

    const int year = 2000 + year_two_digit;

    if (year <= 2022 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
        return false;

    time_data->year = year;
    time_data->month = month;
    time_data->day = day;
    time_data->hour = hour;
    time_data->minute = minute;
    time_data->second = second;
    return true;
}

void gnss_nmea_init(const gnss_nmea_config_t &config)
{
    if (s_rmc_signal == nullptr)
        s_rmc_signal = xSemaphoreCreateBinary();
    s_config = config;
}

bool gnss_nmea_process_sentence(const char *sentence, uint32_t generation)
{
    s_sentences.fetch_add(1, std::memory_order_relaxed);
    if (!gnss_nmea_checksum_valid(sentence))
    {
        s_checksum_errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    if (sentence_is(sentence, "RMC"))
        process_rmc(sentence, generation, now_us);
    else if (sentence_is(sentence, "GGA"))
        process_gga(sentence, now_us);
    else if (sentence_is(sentence, "GSV"))
        process_gsv(sentence, now_us);
    return true;
}

bool gnss_nmea_wait_valid_rmc(uint32_t generation, int64_t deadline_us, nmea_rmc_time_t *time_data)
{
    if (time_data == nullptr || s_rmc_signal == nullptr)
        return false;

    taskENTER_CRITICAL(&s_lock);
    const uint32_t entry_seq = s_rmc.seq;
    taskEXIT_CRITICAL(&s_lock);

    for (;;)
    {
        taskENTER_CRITICAL(&s_lock);
        const rmc_result_t latest = s_rmc;
        taskEXIT_CRITICAL(&s_lock);

        const bool fresh = generation != 0 ? static_cast<int32_t>(latest.generation - generation) >= 0 : latest.seq != entry_seq;
        if (fresh && latest.valid)
        {
            *time_data = latest.time;
            return true;
        }

        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0)
            return false;
        xSemaphoreTake(s_rmc_signal, std::max<TickType_t>(1, pdMS_TO_TICKS(remaining_us / 1000)));
    }
}

void gnss_nmea_set_min_satellites(uint8_t count)
{
    s_min_satellites.store(count, std::memory_order_relaxed);
}

bool gnss_nmea_time_valid(int64_t max_age_us)
{
    taskENTER_CRITICAL(&s_lock);
    const bool valid = s_rmc.valid;
    const int64_t rx_us = s_rmc.rx_us;
    taskEXIT_CRITICAL(&s_lock);
    return valid && rx_us != 0 && esp_timer_get_time() - rx_us <= max_age_us;
}

void gnss_nmea_get_status(gnss_nmea_status_t *status)
{
    if (status == nullptr)
        return;
    *status = gnss_nmea_status_t{};
    status->running = gnss_uart_running();
    status->gga_recent = gga_recent(esp_timer_get_time());
    status->fix_quality = s_gga_fix_quality.load(std::memory_order_relaxed);
    status->satellites_used = s_gga_satellites.load(std::memory_order_relaxed);
    status->rmc_after_pps_ms = s_rmc_phase_ms.load(std::memory_order_relaxed);
    status->rmc_after_pps_min_ms = s_rmc_phase_min_ms.load(std::memory_order_relaxed);
    status->rmc_after_pps_max_ms = s_rmc_phase_max_ms.load(std::memory_order_relaxed);
    status->sentences = s_sentences.load(std::memory_order_relaxed);
    status->checksum_errors = s_checksum_errors.load(std::memory_order_relaxed);
    for (size_t i = 0; i < GNSS_NMEA_RMC_PHASE_BINS; ++i)
        status->rmc_phase_bins[i] = s_rmc_phase_bins[i].load(std::memory_order_relaxed);
    status->overflows = gnss_uart_overflows();
}

#if DEBUG_ENABLED
void gnss_nmea_log_status(void)
{
    if (s_sentences.load(std::memory_order_relaxed) == 0)
        return;
    ESP_LOGI(TAG, "RMC received %ld ms after the PPS (min %ld, max %ld); sentences %lu, checksum errors %lu, overflows %lu%s",
             static_cast<long>(s_rmc_phase_ms.load()), static_cast<long>(s_rmc_phase_min_ms.load()),
             static_cast<long>(s_rmc_phase_max_ms.load()), static_cast<unsigned long>(s_sentences.load()),
             static_cast<unsigned long>(s_checksum_errors.load()), static_cast<unsigned long>(gnss_uart_overflows()),
             gga_recent(esp_timer_get_time()) ? "; GGA present" : "");
}
#endif
