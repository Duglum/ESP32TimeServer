// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "gnss_ubx.h"
#include "gnss_uart.h"
#include "ESP32TimeServerSettings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{
#if DEBUG_ENABLED
    static const char *TAG = "gnss_ubx";
#endif

    // Classes and IDs
    static constexpr uint8_t CLS_NAV = 0x01;
    static constexpr uint8_t CLS_ACK = 0x05;
    static constexpr uint8_t CLS_CFG = 0x06;
    static constexpr uint8_t CLS_MON = 0x0A;
    static constexpr uint8_t CLS_TIM = 0x0D;
    static constexpr uint8_t CLS_SEC = 0x27;
    static constexpr uint8_t ID_NAV_PVT = 0x07;
    static constexpr uint8_t ID_NAV_TIMEUTC = 0x21;
    static constexpr uint8_t ID_ACK_NAK = 0x00;
    static constexpr uint8_t ID_ACK_ACK = 0x01;
    static constexpr uint8_t ID_CFG_VALSET = 0x8A;
    static constexpr uint8_t ID_CFG_VALGET = 0x8B;
    static constexpr uint8_t ID_MON_VER = 0x04;
    static constexpr uint8_t ID_MON_RF = 0x38;
    static constexpr uint8_t ID_TIM_TP = 0x01;
    static constexpr uint8_t ID_SEC_UNIQID = 0x03;

    static constexpr uint16_t NAV_PVT_LENGTH = 92;
    static constexpr uint16_t NAV_TIMEUTC_LENGTH = 20;
    static constexpr uint16_t TIM_TP_LENGTH = 16;
    static constexpr size_t RESPONSE_SIZE = 256;     // MON-VER of an M10: 40 + 30 bytes per extension
    static constexpr size_t MAX_VALSET_ITEMS = 8;
    static constexpr size_t TP_RING_SIZE = 4;        // TIM-TP of the last pulses
    static constexpr int64_t TIMEUTC_MAX_AGE_US = 2000000;
    static constexpr int64_t UNIX_GPS_EPOCH_S = 315964800; // 1980-01-06 00:00:00 UTC
    static constexpr int64_t WEEK_S = 604800;
    static constexpr int16_t LEAP_UNKNOWN = INT16_MIN;

    // ---- Command in progress (one at a time, s_command_mutex); the reader task fills in the answer (s_lock) ----
    struct command_t
    {
        bool active;
        bool want_response; // a message with response_cls / response_id
        uint8_t response_cls;
        uint8_t response_id;
        bool want_ack;      // ACK-ACK / ACK-NAK for ack_cls / ack_id
        uint8_t ack_cls;
        uint8_t ack_id;
        bool got_response;
        int8_t ack_result;  // 0 none yet, 1 ACK, -1 NAK
        uint16_t length;
        uint8_t payload[RESPONSE_SIZE];
    };

    struct timeutc_t
    {
        uint32_t itow_ms;
        uint8_t valid; // bit 2 validUTC
        int64_t rx_us; // 0 = none
    };

    struct tp_t
    {
        uint32_t tow_ms;
        uint32_t tow_sub_ms;
        int32_t qerr_ps;
        uint8_t flags;    // bit 0 time base UTC, bit 4 qErrInvalid
        uint8_t ref_info; // bits 0..3 GNSS of the time base (0 GPS, 1 GLONASS, 2 BeiDou, 3 Galileo)
        bool used;
    };

    portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
    gnss_ubx_config_t s_config{};
    SemaphoreHandle_t s_command_mutex = nullptr;
    SemaphoreHandle_t s_command_signal = nullptr; // given when the answer of the command is complete
    SemaphoreHandle_t s_pvt_signal = nullptr;     // given for every NAV-PVT
    command_t s_command{};

    gnss_ubx_pvt_t s_pvt{};
    timeutc_t s_timeutc{};
    tp_t s_tp[TP_RING_SIZE]{};
    uint32_t s_tp_head = 0;

    std::atomic<bool> s_timeutc_required{false};
    std::atomic<int16_t> s_leap_seconds{LEAP_UNKNOWN};
    std::atomic<uint32_t> s_frames{0};
    std::atomic<uint32_t> s_pvt_messages{0};
    std::atomic<uint32_t> s_timeutc_messages{0};
    std::atomic<uint32_t> s_tp_messages{0};
    std::atomic<uint32_t> s_qerr_matched{0};
    std::atomic<uint32_t> s_qerr_unmatched{0};
    std::atomic<uint32_t> s_qerr_invalid{0};
    std::atomic<uint32_t> s_naks{0};
    std::atomic<int32_t> s_last_qerr_ps{0};
    std::atomic<uint8_t> s_tp_time_base{255};
    std::atomic<int32_t> s_last_tp_qerr_ps{0};   // of the latest TIM-TP, whatever its flags (diagnostics)
    std::atomic<uint8_t> s_last_tp_flags{0};
    // Diagnostics: esp_timer time when NAV-PVT fullyResolved / NAV-TIMEUTC validUTC were first set, and how often
    // they were lost afterwards (reader task only writes)
    std::atomic<int64_t> s_first_resolved_us{0};
    std::atomic<uint32_t> s_resolved_losses{0};
    bool s_resolved = false;
    std::atomic<int64_t> s_first_valid_utc_us{0};
    std::atomic<uint32_t> s_valid_utc_losses{0};
    bool s_valid_utc = false;

    void note_validity(bool now_valid, bool &was_valid, std::atomic<int64_t> &first_us, std::atomic<uint32_t> &losses,
                       int64_t rx_us)
    {
        if (now_valid && first_us.load(std::memory_order_relaxed) == 0)
            first_us.store(rx_us, std::memory_order_relaxed);
        if (!now_valid && was_valid)
            losses.fetch_add(1, std::memory_order_relaxed);
        was_valid = now_valid;
    }

    uint16_t get_u2(const uint8_t *p)
    {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    uint32_t get_u4(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    void put_u4(uint8_t *p, uint32_t value)
    {
        p[0] = static_cast<uint8_t>(value);
        p[1] = static_cast<uint8_t>(value >> 8);
        p[2] = static_cast<uint8_t>(value >> 16);
        p[3] = static_cast<uint8_t>(value >> 24);
    }

    // Size of the value of a configuration key in bytes (bits 28..30: 1 bit, 1, 2, 4 or 8 bytes), 0 = unknown
    size_t key_value_size(uint32_t key)
    {
        switch ((key >> 28) & 0x07)
        {
        case 1:
        case 2:
            return 1;
        case 3:
            return 2;
        case 4:
            return 4;
        case 5:
            return 8;
        default:
            return 0;
        }
    }

    int64_t days_from_civil(int64_t year, unsigned month, unsigned day)
    {
        year -= month <= 2;
        const int64_t era = (year >= 0 ? year : year - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(year - era * 400);
        const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int64_t>(doe) - 719468;
    }

    int64_t positive_modulo(int64_t value, int64_t modulus)
    {
        const int64_t r = value % modulus;
        return r < 0 ? r + modulus : r;
    }

    bool send_frame(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length)
    {
        uint8_t frame[8 + 4 + MAX_VALSET_ITEMS * 12];
        if (length > sizeof(frame) - 8)
            return false;
        frame[0] = 0xB5;
        frame[1] = 0x62;
        frame[2] = cls;
        frame[3] = id;
        frame[4] = static_cast<uint8_t>(length);
        frame[5] = static_cast<uint8_t>(length >> 8);
        if (length > 0)
            memcpy(frame + 6, payload, length);
        uint8_t ck_a = 0;
        uint8_t ck_b = 0;
        for (size_t i = 2; i < 6u + length; ++i)
        {
            ck_a = static_cast<uint8_t>(ck_a + frame[i]);
            ck_b = static_cast<uint8_t>(ck_b + ck_a);
        }
        frame[6 + length] = ck_a;
        frame[7 + length] = ck_b;
        return gnss_uart_write(frame, 8u + length);
    }

    // Sends a command and waits for its answer. want_response: a message of the same class and ID (poll); want_ack:
    // ACK/NAK of the command. Done when the response arrives, when the ACK arrives (without want_response) or a NAK.
    // Returns true for a response or an ACK; the response stays in s_command until the mutex is given back.
    bool run_command(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length, bool want_response, bool want_ack,
                     uint32_t timeout_ms)
    {
        if (s_command_signal == nullptr || !gnss_uart_running())
            return false;

        taskENTER_CRITICAL(&s_lock);
        s_command.active = true;
        s_command.want_response = want_response;
        s_command.response_cls = cls;
        s_command.response_id = id;
        s_command.want_ack = want_ack;
        s_command.ack_cls = cls;
        s_command.ack_id = id;
        s_command.got_response = false;
        s_command.ack_result = 0;
        s_command.length = 0;
        taskEXIT_CRITICAL(&s_lock);
        (void)xSemaphoreTake(s_command_signal, 0);

        bool result = false;
        if (send_frame(cls, id, payload, length))
        {
            const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000LL;
            for (;;)
            {
                taskENTER_CRITICAL(&s_lock);
                const bool got_response = s_command.got_response;
                const int8_t ack_result = s_command.ack_result;
                taskEXIT_CRITICAL(&s_lock);
                if (want_response ? got_response : ack_result > 0)
                {
                    result = true;
                    break;
                }
                if (ack_result < 0)
                    break;
                const int64_t remaining_us = deadline_us - esp_timer_get_time();
                if (remaining_us <= 0)
                    break;
                xSemaphoreTake(s_command_signal, std::max<TickType_t>(1, pdMS_TO_TICKS(remaining_us / 1000)));
            }
        }

        taskENTER_CRITICAL(&s_lock);
        s_command.active = false;
        taskEXIT_CRITICAL(&s_lock);
        return result;
    }

    void process_ack(uint8_t id, const uint8_t *payload, uint16_t length)
    {
        if (length < 2)
            return;
        if (id == ID_ACK_NAK)
            s_naks.fetch_add(1, std::memory_order_relaxed);
        bool signal = false;
        taskENTER_CRITICAL(&s_lock);
        if (s_command.active && s_command.want_ack && s_command.ack_result == 0 && payload[0] == s_command.ack_cls &&
            payload[1] == s_command.ack_id)
        {
            s_command.ack_result = id == ID_ACK_ACK ? 1 : -1;
            signal = true;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (signal)
            xSemaphoreGive(s_command_signal);
    }

    void process_response(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length)
    {
        bool signal = false;
        taskENTER_CRITICAL(&s_lock);
        if (s_command.active && s_command.want_response && !s_command.got_response && cls == s_command.response_cls &&
            id == s_command.response_id && length > 0)
        {
            s_command.length = static_cast<uint16_t>(std::min<size_t>(length, RESPONSE_SIZE));
            memcpy(s_command.payload, payload, s_command.length);
            s_command.got_response = true;
            signal = true;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (signal)
            xSemaphoreGive(s_command_signal);
    }

    // GPS - UTC in whole seconds from a NAV-PVT with a resolved UTC time (time pulses on the GNSS time base)
    void note_leap_seconds(const gnss_ubx_pvt_t &pvt)
    {
        if ((pvt.valid & 0x07) != 0x07 || pvt.month < 1 || pvt.month > 12)
            return;
        int64_t utc_s = days_from_civil(pvt.year, pvt.month, pvt.day) * 86400 + pvt.hour * 3600 + pvt.minute * 60 + pvt.second;
        if (pvt.nano >= 500000000)
            ++utc_s;
        else if (pvt.nano < -500000000)
            --utc_s;
        const int64_t utc_tow_s = positive_modulo(utc_s - UNIX_GPS_EPOCH_S, WEEK_S);
        const int64_t gps_tow_s = (static_cast<int64_t>(pvt.itow_ms) + 500) / 1000;
        int64_t leap = positive_modulo(gps_tow_s - utc_tow_s, WEEK_S);
        if (leap > WEEK_S / 2)
            leap -= WEEK_S;
        if (leap >= 0 && leap <= 255)
            s_leap_seconds.store(static_cast<int16_t>(leap), std::memory_order_relaxed);
    }

    void process_nav_pvt(const uint8_t *p, uint16_t length, uint32_t generation)
    {
        if (length < NAV_PVT_LENGTH)
            return;
        gnss_ubx_pvt_t pvt{};
        pvt.itow_ms = get_u4(p + 0);
        pvt.year = get_u2(p + 4);
        pvt.month = p[6];
        pvt.day = p[7];
        pvt.hour = p[8];
        pvt.minute = p[9];
        pvt.second = p[10];
        pvt.valid = p[11];
        pvt.nano = static_cast<int32_t>(get_u4(p + 16));
        pvt.fix_type = p[20];
        pvt.flags = p[21];
        pvt.flags2 = p[22];
        pvt.num_sv = p[23];
        pvt.rx_us = esp_timer_get_time();
        pvt.generation = generation;

        taskENTER_CRITICAL(&s_lock);
        s_pvt = pvt;
        taskEXIT_CRITICAL(&s_lock);
        s_pvt_messages.fetch_add(1, std::memory_order_relaxed);
        note_validity((pvt.valid & 0x04) != 0, s_resolved, s_first_resolved_us, s_resolved_losses, pvt.rx_us);
        note_leap_seconds(pvt);
        if (s_pvt_signal != nullptr)
            xSemaphoreGive(s_pvt_signal);
        if (s_config.on_satellites != nullptr)
            s_config.on_satellites(pvt.num_sv);
    }

    void process_nav_timeutc(const uint8_t *p, uint16_t length)
    {
        if (length < NAV_TIMEUTC_LENGTH)
            return;
        timeutc_t timeutc{};
        timeutc.itow_ms = get_u4(p + 0);
        timeutc.valid = p[19];
        timeutc.rx_us = esp_timer_get_time();
        taskENTER_CRITICAL(&s_lock);
        s_timeutc = timeutc;
        taskEXIT_CRITICAL(&s_lock);
        s_timeutc_messages.fetch_add(1, std::memory_order_relaxed);
        note_validity((timeutc.valid & 0x04) != 0, s_valid_utc, s_first_valid_utc_us, s_valid_utc_losses, timeutc.rx_us);
    }

    // TIM-TP describes the next time pulse (u-blox application note GPS.G6-X-11007): kept in a ring, the PPS task looks
    // up the pulse it measured by its time of week
    void process_tim_tp(const uint8_t *p, uint16_t length)
    {
        if (length < TIM_TP_LENGTH)
            return;
        tp_t tp{};
        tp.tow_ms = get_u4(p + 0);
        tp.tow_sub_ms = get_u4(p + 4);
        tp.qerr_ps = static_cast<int32_t>(get_u4(p + 8));
        tp.flags = p[14];
        tp.ref_info = p[15];
        tp.used = true;
        taskENTER_CRITICAL(&s_lock);
        s_tp[s_tp_head % TP_RING_SIZE] = tp;
        s_tp_head++;
        taskEXIT_CRITICAL(&s_lock);
        s_tp_messages.fetch_add(1, std::memory_order_relaxed);
        s_tp_time_base.store(tp.flags & 0x01, std::memory_order_relaxed);
        s_last_tp_qerr_ps.store(tp.qerr_ps, std::memory_order_relaxed);
        s_last_tp_flags.store(tp.flags, std::memory_order_relaxed);
    }
}

void gnss_ubx_init(const gnss_ubx_config_t &config)
{
    if (s_command_mutex == nullptr)
        s_command_mutex = xSemaphoreCreateMutex();
    if (s_command_signal == nullptr)
        s_command_signal = xSemaphoreCreateBinary();
    if (s_pvt_signal == nullptr)
        s_pvt_signal = xSemaphoreCreateBinary();
    taskENTER_CRITICAL(&s_lock);
    s_config = config;
    s_pvt = gnss_ubx_pvt_t{};
    s_timeutc = timeutc_t{};
    for (tp_t &tp : s_tp)
        tp = tp_t{};
    taskEXIT_CRITICAL(&s_lock);
    s_timeutc_required.store(false, std::memory_order_relaxed);
}

void gnss_ubx_process_frame(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length, uint32_t generation)
{
    s_frames.fetch_add(1, std::memory_order_relaxed);
    if (cls == CLS_NAV && id == ID_NAV_PVT)
        process_nav_pvt(payload, length, generation);
    else if (cls == CLS_NAV && id == ID_NAV_TIMEUTC)
        process_nav_timeutc(payload, length);
    else if (cls == CLS_TIM && id == ID_TIM_TP)
        process_tim_tp(payload, length);
    else if (cls == CLS_ACK)
        process_ack(id, payload, length);
    else
        process_response(cls, id, payload, length);
}

bool gnss_ubx_val_set(const gnss_ubx_cfg_item_t *items, size_t count, uint8_t layers, uint32_t timeout_ms)
{
    if (items == nullptr || count == 0 || count > MAX_VALSET_ITEMS || s_command_mutex == nullptr)
        return false;
    uint8_t payload[4 + MAX_VALSET_ITEMS * 8];
    size_t length = 0;
    payload[length++] = 0; // version 0: RAM, BBR and flash in one transaction
    payload[length++] = layers;
    payload[length++] = 0;
    payload[length++] = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const size_t size = key_value_size(items[i].key);
        if (size == 0 || size > 4)
            return false;
        put_u4(payload + length, items[i].key);
        length += 4;
        for (size_t b = 0; b < size; ++b)
            payload[length++] = static_cast<uint8_t>(items[i].value >> (8 * b));
    }

    xSemaphoreTake(s_command_mutex, portMAX_DELAY);
    const bool ok = run_command(CLS_CFG, ID_CFG_VALSET, payload, static_cast<uint16_t>(length), false, true, timeout_ms);
    xSemaphoreGive(s_command_mutex);
    return ok;
}

bool gnss_ubx_val_get(uint32_t key, uint8_t layer, uint32_t *value, uint32_t timeout_ms)
{
    const size_t size = key_value_size(key);
    if (value == nullptr || size == 0 || size > 4 || s_command_mutex == nullptr)
        return false;
    uint8_t payload[8] = {0, static_cast<uint8_t>(layer == GNSS_UBX_LAYER_RAM ? 0 : (layer == GNSS_UBX_LAYER_BBR ? 1 : 7)), 0, 0};
    put_u4(payload + 4, key);

    bool ok = false;
    xSemaphoreTake(s_command_mutex, portMAX_DELAY);
    if (run_command(CLS_CFG, ID_CFG_VALGET, payload, sizeof(payload), true, true, timeout_ms))
    {
        // Answer: version, layer, position (2 bytes), then key and value
        const uint8_t *p = s_command.payload;
        if (s_command.length >= 8 + size && get_u4(p + 4) == key)
        {
            uint32_t v = 0;
            for (size_t b = 0; b < size; ++b)
                v |= static_cast<uint32_t>(p[8 + b]) << (8 * b);
            *value = v;
            ok = true;
        }
    }
    xSemaphoreGive(s_command_mutex);
    return ok;
}

bool gnss_ubx_query_rf(gnss_ubx_rf_t *rf, uint32_t timeout_ms)
{
    if (rf == nullptr || s_command_mutex == nullptr)
        return false;
    bool ok = false;
    xSemaphoreTake(s_command_mutex, portMAX_DELAY);
    // version, nBlocks, 2 reserved bytes, then 24 bytes per RF block
    if (run_command(CLS_MON, ID_MON_RF, nullptr, 0, true, false, timeout_ms) && s_command.length >= 4 + 24 &&
        s_command.payload[1] >= 1)
    {
        const uint8_t *b = s_command.payload + 4;
        rf->jamming_state = b[1] & 0x03;
        rf->antenna_status = b[2];
        rf->antenna_power = b[3];
        rf->noise_per_ms = get_u2(b + 12);
        rf->agc_count = get_u2(b + 14);
        rf->jam_indicator = b[16];
        ok = true;
    }
    xSemaphoreGive(s_command_mutex);
    return ok;
}

bool gnss_ubx_query_unique_id(char *text, size_t text_size, uint32_t timeout_ms)
{
    if (text == nullptr || text_size < 13 || s_command_mutex == nullptr)
        return false;
    bool ok = false;
    xSemaphoreTake(s_command_mutex, portMAX_DELAY);
    if (run_command(CLS_SEC, ID_SEC_UNIQID, nullptr, 0, true, false, timeout_ms) && s_command.length >= 9)
    {
        // version, 3 reserved bytes, unique ID: 5 bytes (version 1, F9 and M9) or 6 bytes (version 2, M10)
        const uint8_t *p = s_command.payload;
        const uint8_t version = p[0];
        uint8_t id[6] = {p[4], p[5], p[6], p[7], p[8], 0};
        if (version == 2 && s_command.length == 10)
            id[5] = p[9];
        const size_t bytes = std::min<size_t>(static_cast<size_t>(version) + 4, 6);
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < bytes; ++i)
        {
            text[i * 2] = hex[id[i] >> 4];
            text[i * 2 + 1] = hex[id[i] & 0x0F];
        }
        text[bytes * 2] = '\0';
        ok = bytes > 0;
    }
    xSemaphoreGive(s_command_mutex);
    return ok;
}

bool gnss_ubx_query_version(gnss_ubx_version_t *version, uint32_t timeout_ms)
{
    if (version == nullptr || s_command_mutex == nullptr)
        return false;
    *version = gnss_ubx_version_t{};

    // "<high>.<low>" (as sscanf("%d.%d") of the SparkFun library: spaces before the number are skipped)
    const auto parse_version = [](const char *text, uint8_t &high, uint8_t &low) -> bool
    {
        while (*text == ' ')
            ++text;
        unsigned int values[2] = {0, 0};
        for (int part = 0; part < 2; ++part)
        {
            if (*text < '0' || *text > '9')
                return false;
            while (*text >= '0' && *text <= '9')
                values[part] = values[part] * 10 + static_cast<unsigned int>(*text++ - '0');
            if (part == 0 && *text++ != '.')
                return false;
        }
        high = static_cast<uint8_t>(values[0]);
        low = static_cast<uint8_t>(values[1]);
        return true;
    };

    uint8_t found = 0; // bit 0 FWVER, bit 1 PROTVER, bit 2 MOD
    xSemaphoreTake(s_command_mutex, portMAX_DELAY);
    if (run_command(CLS_MON, ID_MON_VER, nullptr, 0, true, false, timeout_ms) && s_command.length >= 40)
    {
        // swVersion (30 bytes), hwVersion (10 bytes), then extensions of 30 bytes each
        for (size_t offset = 40; offset + 30 <= s_command.length; offset += 30)
        {
            char extension[31];
            memcpy(extension, s_command.payload + offset, 30);
            extension[30] = '\0';
            const char *ptr = strstr(extension, "FWVER=");
            if (ptr != nullptr)
            {
                ptr += 6;
                size_t i = 0;
                while (i < sizeof(version->firmware_type) - 1 && *ptr != '\0' && *ptr != ' ')
                    version->firmware_type[i++] = *ptr++;
                version->firmware_type[i] = '\0';
                if (*ptr == ' ')
                    ++ptr;
                if (parse_version(ptr, version->firmware_high, version->firmware_low))
                    found |= 0x01;
            }
            ptr = strstr(extension, "PROTVER=");
            if (ptr != nullptr && parse_version(ptr + 8, version->protocol_high, version->protocol_low))
                found |= 0x02;
            ptr = strstr(extension, "MOD=");
            if (ptr != nullptr)
            {
                ptr += 4;
                size_t i = 0;
                while (i < sizeof(version->module) - 1 && *ptr != '\0' && *ptr != ' ')
                    version->module[i++] = *ptr++;
                version->module[i] = '\0';
                found |= 0x04;
            }
        }
    }
    xSemaphoreGive(s_command_mutex);

    if ((found & 0x04) == 0) // the UBX-M10050-KB chip reports no MOD
        snprintf(version->module, sizeof(version->module), "NONE");
    return (found & 0x03) == 0x03;
}

bool gnss_ubx_latest_pvt(gnss_ubx_pvt_t *pvt, int64_t max_age_us)
{
    if (pvt == nullptr)
        return false;
    taskENTER_CRITICAL(&s_lock);
    *pvt = s_pvt;
    taskEXIT_CRITICAL(&s_lock);
    return pvt->rx_us != 0 && esp_timer_get_time() - pvt->rx_us <= max_age_us;
}

bool gnss_ubx_wait_pvt(uint32_t generation, int64_t deadline_us, gnss_ubx_pvt_t *pvt)
{
    if (pvt == nullptr || s_pvt_signal == nullptr)
        return false;
    for (;;)
    {
        taskENTER_CRITICAL(&s_lock);
        const gnss_ubx_pvt_t latest = s_pvt;
        taskEXIT_CRITICAL(&s_lock);
        if (latest.rx_us != 0 && static_cast<int32_t>(latest.generation - generation) >= 0)
        {
            *pvt = latest;
            return true;
        }
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0)
            return false;
        xSemaphoreTake(s_pvt_signal, std::max<TickType_t>(1, pdMS_TO_TICKS(remaining_us / 1000)));
    }
}

uint32_t gnss_ubx_pvt_invalid_reasons(const gnss_ubx_pvt_t &pvt, uint8_t min_satellites)
{
    uint32_t reasons = 0;
    if ((pvt.fix_type != 3 && pvt.fix_type != 4 && pvt.fix_type != 5) || (pvt.flags & 0x01) == 0)
        reasons |= GNSS_UBX_INVALID_FIX;
    if ((pvt.valid & 0x03) != 0x03)
        reasons |= GNSS_UBX_INVALID_DATE_TIME;
    // validTime alone does not mean that the leap seconds are known: after a cold start the UTC time can be off by
    // whole seconds until it is fully resolved
    if ((pvt.valid & 0x04) == 0)
        reasons |= GNSS_UBX_INVALID_NOT_RESOLVED;
    // confirmedTime is only required when the receiver reports whether it could confirm the time (confirmedAvai)
    if ((pvt.flags2 & 0x20) != 0 && (pvt.flags2 & 0x80) == 0)
        reasons |= GNSS_UBX_INVALID_NOT_CONFIRMED;
    if (min_satellites != 0 && pvt.num_sv < min_satellites)
        reasons |= GNSS_UBX_INVALID_SATELLITES;
    if (s_timeutc_required.load(std::memory_order_relaxed))
    {
        // NAV-TIMEUTC of the same navigation epoch (or a recent one): the UTC parameters are known (validUTC)
        taskENTER_CRITICAL(&s_lock);
        const timeutc_t timeutc = s_timeutc;
        taskEXIT_CRITICAL(&s_lock);
        const bool recent = timeutc.rx_us != 0 &&
                            (timeutc.itow_ms == pvt.itow_ms || std::llabs(pvt.rx_us - timeutc.rx_us) <= TIMEUTC_MAX_AGE_US);
        if (!recent || (timeutc.valid & 0x04) == 0)
            reasons |= GNSS_UBX_INVALID_UTC;
    }
    return reasons;
}

void gnss_ubx_set_timeutc_required(bool required)
{
    s_timeutc_required.store(required, std::memory_order_relaxed);
}

bool gnss_ubx_qerr_for_edge(int64_t edge_unix_sec, int32_t *qerr_ps)
{
    if (qerr_ps == nullptr)
        return false;
    // Time of week of the pulse on its time base: UTC directly; GPS and Galileo time are UTC + leap seconds
    const int64_t utc_tow_ms = positive_modulo(edge_unix_sec - UNIX_GPS_EPOCH_S, WEEK_S) * 1000;
    const int16_t leap = s_leap_seconds.load(std::memory_order_relaxed);
    const int64_t gnss_tow_ms = leap != LEAP_UNKNOWN ? positive_modulo(utc_tow_ms / 1000 + leap, WEEK_S) * 1000 : -1;

    // Looked up in place (no copy of the ring: the PPS task has a small stack)
    int found = 0; // 0 none in the ring, 1 no match, 2 match, 3 match flagged qErrInvalid
    int32_t qerr = 0;
    taskENTER_CRITICAL(&s_lock);
    for (const tp_t &tp : s_tp)
    {
        if (!tp.used)
            continue;
        found = std::max(found, 1);
        const uint8_t gnss = tp.ref_info & 0x0F;
        const int64_t expected_ms = (tp.flags & 0x01) != 0 ? utc_tow_ms : ((gnss == 0 || gnss == 3) ? gnss_tow_ms : -1);
        if (expected_ms < 0 || static_cast<int64_t>(tp.tow_ms) != expected_ms)
            continue;
        found = (tp.flags & 0x10) != 0 ? 3 : 2;
        qerr = tp.qerr_ps;
        break;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (found == 1)
        s_qerr_unmatched.fetch_add(1, std::memory_order_relaxed);
    else if (found == 3)
        s_qerr_invalid.fetch_add(1, std::memory_order_relaxed);
    if (found != 2)
        return false;
    *qerr_ps = qerr;
    s_last_qerr_ps.store(qerr, std::memory_order_relaxed);
    s_qerr_matched.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void gnss_ubx_get_status(gnss_ubx_status_t *status)
{
    if (status == nullptr)
        return;
    *status = gnss_ubx_status_t{};
    status->frames = s_frames.load(std::memory_order_relaxed);
    status->pvt_messages = s_pvt_messages.load(std::memory_order_relaxed);
    status->timeutc_messages = s_timeutc_messages.load(std::memory_order_relaxed);
    status->tp_messages = s_tp_messages.load(std::memory_order_relaxed);
    status->qerr_matched = s_qerr_matched.load(std::memory_order_relaxed);
    status->qerr_unmatched = s_qerr_unmatched.load(std::memory_order_relaxed);
    status->qerr_invalid = s_qerr_invalid.load(std::memory_order_relaxed);
    status->naks = s_naks.load(std::memory_order_relaxed);
    status->last_qerr_ps = s_last_qerr_ps.load(std::memory_order_relaxed);
    status->leap_seconds = s_leap_seconds.load(std::memory_order_relaxed);
    status->tp_time_base = s_tp_time_base.load(std::memory_order_relaxed);
    status->last_tp_qerr_ps = s_last_tp_qerr_ps.load(std::memory_order_relaxed);
    status->last_tp_flags = s_last_tp_flags.load(std::memory_order_relaxed);
    status->first_resolved_us = s_first_resolved_us.load(std::memory_order_relaxed);
    status->resolved_losses = s_resolved_losses.load(std::memory_order_relaxed);
    status->first_valid_utc_us = s_first_valid_utc_us.load(std::memory_order_relaxed);
    status->valid_utc_losses = s_valid_utc_losses.load(std::memory_order_relaxed);
}

#if DEBUG_ENABLED
void gnss_ubx_log_status(void)
{
    gnss_ubx_status_t status{};
    gnss_ubx_get_status(&status);
    if (status.frames == 0)
        return;
    ESP_LOGI(TAG, "UBX frames %lu (NAV-PVT %lu, NAV-TIMEUTC %lu, TIM-TP %lu, NAK %lu); qErr matched %lu, unmatched %lu, invalid %lu, last %ld ps; leap %d s",
             static_cast<unsigned long>(status.frames), static_cast<unsigned long>(status.pvt_messages),
             static_cast<unsigned long>(status.timeutc_messages), static_cast<unsigned long>(status.tp_messages),
             static_cast<unsigned long>(status.naks), static_cast<unsigned long>(status.qerr_matched),
             static_cast<unsigned long>(status.qerr_unmatched), static_cast<unsigned long>(status.qerr_invalid),
             static_cast<long>(status.last_qerr_ps), static_cast<int>(status.leap_seconds));
}
#endif
