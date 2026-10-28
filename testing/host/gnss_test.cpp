// ESP32 Time Server - host tests
// GNSS input: the byte parser of the UART reader (main/custom/gnss_uart.cpp), the NMEA sentences
// (main/custom/gnss_nmea.cpp) and the UBX messages and commands (main/custom/gnss_ubx.cpp).
// Built twice by run_tests.ps1: as a test program (main) and, with HOST_FUZZ, as a libFuzzer target.

#include "gnss_nmea.h"
#include "gnss_uart.h"
#include "gnss_ubx.h"
#include "host_hooks.h"
#include "host_stubs.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using bytes_t = std::vector<uint8_t>;

    uint32_t s_generation = 1;
    int s_satellites_reported = -1;

    void on_satellites(uint8_t count)
    {
        s_satellites_reported = count;
    }

    int64_t s_last_pps_us = 0;

    int64_t last_pps_us()
    {
        return s_last_pps_us;
    }

    // ---- Frames and sentences ----
    std::string nmea(const std::string &body)
    {
        uint8_t checksum = 0;
        for (char c : body)
            checksum ^= static_cast<uint8_t>(c);
        char tail[8];
        std::snprintf(tail, sizeof(tail), "*%02X\r\n", checksum);
        return "$" + body + tail;
    }

    bytes_t ubx(uint8_t cls, uint8_t id, const bytes_t &payload)
    {
        bytes_t frame = {0xB5, 0x62, cls, id, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8)};
        frame.insert(frame.end(), payload.begin(), payload.end());
        uint8_t a = 0;
        uint8_t b = 0;
        for (size_t i = 2; i < frame.size(); ++i)
        {
            a = static_cast<uint8_t>(a + frame[i]);
            b = static_cast<uint8_t>(b + a);
        }
        frame.push_back(a);
        frame.push_back(b);
        return frame;
    }

    void put_u2(bytes_t &p, size_t offset, uint16_t value)
    {
        p[offset] = static_cast<uint8_t>(value);
        p[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    void put_u4(bytes_t &p, size_t offset, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            p[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }

    void feed(const bytes_t &data, uint32_t generation = s_generation)
    {
        host_gnss_uart_feed(data.data(), data.size(), generation);
    }

    void feed(const std::string &text, uint32_t generation = s_generation)
    {
        host_gnss_uart_feed(reinterpret_cast<const uint8_t *>(text.data()), text.size(), generation);
    }

    // ---- Answers of the simulated receiver to commands (sent through gnss_uart_write) ----
    struct answer_t
    {
        bool active = false;
        uint8_t cls = 0;
        uint8_t id = 0;
        bytes_t payload;
        bool nak = false;   // ACK-NAK instead of an answer
        bool ack = false;   // ACK-ACK after the answer (CFG-VALGET, CFG-VALSET)
    } s_answer;
    bytes_t s_last_command;

    void responder(const uint8_t *data, size_t length)
    {
        s_last_command.assign(data, data + length);
        if (!s_answer.active || length < 4)
            return;
        const uint8_t cls = data[2];
        const uint8_t id = data[3];
        if (s_answer.nak)
        {
            feed(ubx(0x05, 0x00, {cls, id}));
            return;
        }
        if (!s_answer.payload.empty())
            feed(ubx(s_answer.cls, s_answer.id, s_answer.payload));
        if (s_answer.ack)
            feed(ubx(0x05, 0x01, {cls, id}));
    }

    void start(bool running = true)
    {
        host_now_us += 60000000; // older GGA, GSV and messages of an earlier test have expired
        ++s_generation;
        s_satellites_reported = -1;
        s_last_pps_us = 0;
        s_answer = answer_t{};
        s_last_command.clear();
        host_gnss_uart_reset(running);
        gnss_nmea_init(gnss_nmea_config_t{last_pps_us, on_satellites});
        gnss_nmea_set_min_satellites(0);
        gnss_ubx_init(gnss_ubx_config_t{on_satellites});
        host_uart_tx_hook = responder;
    }

    const std::string RMC_VALID = "GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.012,,281026,,,A,V";

    bytes_t nav_pvt(uint32_t itow_ms, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                    uint8_t second, uint8_t valid, uint8_t fix_type, uint8_t flags, uint8_t num_sv)
    {
        bytes_t p(92, 0);
        put_u4(p, 0, itow_ms);
        put_u2(p, 4, year);
        p[6] = month;
        p[7] = day;
        p[8] = hour;
        p[9] = minute;
        p[10] = second;
        p[11] = valid;
        p[20] = fix_type;
        p[21] = flags;
        p[22] = 0;
        p[23] = num_sv;
        return p;
    }

    // ---------------------------------------------------------------------------------------------------------------

    void test_nmea_checksum()
    {
        const std::string sentence = nmea(RMC_VALID);
        const std::string line = sentence.substr(0, sentence.size() - 2); // without CR LF
        CHECK(gnss_nmea_checksum_valid(line.c_str()));
        std::string lower = line;
        std::transform(lower.end() - 2, lower.end(), lower.end() - 2, [](char c) { return static_cast<char>(std::tolower(c)); });
        CHECK(gnss_nmea_checksum_valid(lower.c_str()));
        std::string wrong = line;
        wrong[10] = wrong[10] == '1' ? '2' : '1';
        CHECK(!gnss_nmea_checksum_valid(wrong.c_str()));
        CHECK(!gnss_nmea_checksum_valid((line + "0").c_str()));          // three characters after '*'
        CHECK(!gnss_nmea_checksum_valid(line.substr(0, line.size() - 1).c_str()));
        CHECK(!gnss_nmea_checksum_valid(line.substr(0, line.size() - 3).c_str())); // no checksum
        CHECK(!gnss_nmea_checksum_valid(line.substr(1).c_str()));        // no '$'
        CHECK(gnss_nmea_checksum_valid("$*00"));                          // an empty body has the checksum 00
        CHECK(!gnss_nmea_checksum_valid(nullptr));
    }

    void test_nmea_rmc_parse()
    {
        nmea_rmc_time_t t{};
        CHECK(gnss_nmea_parse_rmc(nmea(RMC_VALID).c_str(), &t));
        CHECK_EQ(t.year, 2026);
        CHECK_EQ(t.month, 10);
        CHECK_EQ(t.day, 28);
        CHECK_EQ(t.hour, 12);
        CHECK_EQ(t.minute, 35);
        CHECK_EQ(t.second, 19);

        // Status V, mode N / E, a year before the build, impossible values, short fields
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,123519.00,V,,,,,,,281026,,,N,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.012,,281026,,,N,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.012,,281026,,,E,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.012,,281025,,,A,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,243519.00,A,4807.03800,N,01131.00000,E,0.012,,281026,,,A,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,123519.00,A,4807.03800,N,01131.00000,E,0.012,,281326,,,A,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,1235,A,4807.03800,N,01131.00000,E,0.012,,281026,,,A,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNRMC,12a519.00,A,4807.03800,N,01131.00000,E,0.012,,281026,,,A,V").c_str(), &t));
        CHECK(!gnss_nmea_parse_rmc(nmea("GNGGA,123519.00,A").c_str(), &t));
        // Leap second and a receiver without the mode field (NMEA 2.2)
        CHECK(gnss_nmea_parse_rmc(nmea("GPRMC,235960,A,4807.038,N,01131.000,E,022.4,084.4,311226,003.1,W").c_str(), &t));
        CHECK_EQ(t.second, 60);
    }

    void test_uart_nmea_framing()
    {
        start();
        // Noise and a cut sentence before, binary bytes after
        feed(std::string("garbage\r\n$GNRMC,12"));
        feed(nmea(RMC_VALID));
        feed(bytes_t{0x00, 0xFF, 0x7F});
        CHECK(gnss_nmea_time_valid(1000000));
        gnss_uart_traffic_t traffic{};
        gnss_uart_get_traffic(&traffic);
        CHECK_EQ(traffic.nmea_sentences, 1);

        nmea_rmc_time_t t{};
        CHECK(gnss_nmea_wait_valid_rmc(s_generation, host_now_us, &t));
        CHECK_EQ(t.second, 19);

        // A flush (new generation) in the middle of a sentence drops it
        start();
        const std::string sentence = nmea(RMC_VALID);
        feed(sentence.substr(0, 20), s_generation);
        feed(sentence.substr(20), s_generation + 1);
        CHECK(!gnss_nmea_time_valid(1000000));

        // A UBX sync byte inside a sentence drops it; a too long sentence is dropped
        start();
        feed(sentence.substr(0, 20) + "\xB5" + sentence.substr(20));
        CHECK(!gnss_nmea_time_valid(1000000));
        feed("$GNTXT," + std::string(200, 'A') + "\r\n");
        feed(sentence);
        CHECK(gnss_nmea_time_valid(1000000));
    }

    void test_nmea_gga_and_min_satellites()
    {
        start();
        feed(nmea("GNGGA,123519.00,4807.03800,N,01131.00000,E,1,12,0.9,545.4,M,46.9,M,,"));
        CHECK_EQ(s_satellites_reported, 12);
        gnss_nmea_status_t status{};
        gnss_nmea_get_status(&status);
        CHECK(status.gga_recent);
        CHECK_EQ(status.fix_quality, 1);
        CHECK_EQ(status.satellites_used, 12);

        feed(nmea(RMC_VALID));
        CHECK(gnss_nmea_time_valid(1000000));
        gnss_nmea_set_min_satellites(13);
        feed(nmea(RMC_VALID));
        CHECK(!gnss_nmea_time_valid(1000000));
        gnss_nmea_set_min_satellites(12);
        feed(nmea(RMC_VALID));
        CHECK(gnss_nmea_time_valid(1000000));

        // Fix quality 6 (dead reckoning) while GGA arrives: the RMC does not count
        feed(nmea("GNGGA,123520.00,4807.03800,N,01131.00000,E,6,12,0.9,545.4,M,46.9,M,,"));
        feed(nmea(RMC_VALID));
        CHECK(!gnss_nmea_time_valid(1000000));

        // A minimum without GGA: not valid; after the GGA expired the RMC alone counts again (minimum 0)
        host_now_us += 3000000;
        feed(nmea(RMC_VALID));
        CHECK(!gnss_nmea_time_valid(1000000));
        gnss_nmea_set_min_satellites(0);
        feed(nmea(RMC_VALID));
        CHECK(gnss_nmea_time_valid(1000000));
    }

    std::string gsv(const char *talker, int count, int number, int in_view, const char *signal_id)
    {
        std::string body = std::string(talker) + "GSV," + std::to_string(count) + "," + std::to_string(number) + "," +
                           std::to_string(in_view);
        const int in_message = std::max(0, std::min(4, in_view - (number - 1) * 4));
        for (int i = 0; i < in_message; ++i)
            body += "," + std::to_string(10 + (number - 1) * 4 + i) + ",45,123,40";
        if (signal_id != nullptr)
            body += std::string(",") + signal_id;
        return nmea(body);
    }

    void test_nmea_gsv()
    {
        start();
        // GPS 11 satellites in 3 messages, GLONASS 7 in 2, both with a signal ID (NMEA 4.1)
        feed(gsv("GP", 3, 1, 11, "1"));
        feed(gsv("GP", 3, 2, 11, "1"));
        CHECK_EQ(s_satellites_reported, -1); // the group is not complete yet
        feed(gsv("GP", 3, 3, 11, "1"));
        CHECK_EQ(s_satellites_reported, 11);
        feed(gsv("GL", 2, 1, 7, "1"));
        feed(gsv("GL", 2, 2, 7, "1"));
        CHECK_EQ(s_satellites_reported, 18);
        // A combined GN group is not added to the constellation groups
        feed(gsv("GN", 1, 1, 3, "1"));
        CHECK_EQ(s_satellites_reported, 18);
        // Messages out of order are ignored
        feed(gsv("GA", 2, 2, 6, "7"));
        CHECK_EQ(s_satellites_reported, 18);
        // Malformed: number above count, zero count
        feed(gsv("GB", 1, 2, 6, "1"));
        feed(gsv("GB", 0, 0, 6, "1"));
        CHECK_EQ(s_satellites_reported, 18);
        // While GGA arrives, GSV is not reported
        feed(nmea("GNGGA,123519.00,4807.03800,N,01131.00000,E,1,9,0.9,545.4,M,46.9,M,,"));
        CHECK_EQ(s_satellites_reported, 9);
        feed(gsv("GQ", 1, 1, 2, "1"));
        CHECK_EQ(s_satellites_reported, 9);
    }

    void test_nmea_rmc_phase()
    {
        start();
        s_last_pps_us = host_now_us - 345000; // RMC 345 ms after the PPS edge
        feed(nmea(RMC_VALID));
        gnss_nmea_status_t status{};
        gnss_nmea_get_status(&status);
        CHECK_EQ(status.rmc_after_pps_ms, 345);
        CHECK(status.rmc_phase_bins[3] >= 1);
        s_last_pps_us = host_now_us - 1500000; // a second without an edge: not counted
        feed(nmea(RMC_VALID));
        gnss_nmea_get_status(&status);
        CHECK_EQ(status.rmc_after_pps_ms, 345);
    }

    void test_ubx_nav_pvt()
    {
        start();
        // 2026-10-28 12:00:00 UTC = GPS time of week 302418 s (18 leap seconds)
        const bytes_t pvt = nav_pvt(302418000, 2026, 10, 28, 12, 0, 0, 0x07, 3, 0x01, 14);
        // Noise, an NMEA sentence and a frame that is too long for the parser around it
        feed(std::string("\x01\x02$GNTXT,01*00\r\n"));
        bytes_t big = ubx(0x0A, 0x31, bytes_t(600, 0x55));
        feed(big);
        feed(ubx(0x01, 0x07, pvt));
        gnss_ubx_pvt_t latest{};
        CHECK(gnss_ubx_latest_pvt(&latest, 1000000));
        CHECK_EQ(latest.itow_ms, 302418000);
        CHECK_EQ(latest.year, 2026);
        CHECK_EQ(latest.month, 10);
        CHECK_EQ(latest.day, 28);
        CHECK_EQ(latest.hour, 12);
        CHECK_EQ(latest.num_sv, 14);
        CHECK_EQ(latest.generation, s_generation);
        CHECK_EQ(s_satellites_reported, 14);
        CHECK_EQ(gnss_ubx_pvt_invalid_reasons(latest, 0), 0);
        CHECK_EQ(gnss_ubx_pvt_invalid_reasons(latest, 15), GNSS_UBX_INVALID_SATELLITES);
        gnss_ubx_status_t status{};
        gnss_ubx_get_status(&status);
        CHECK_EQ(status.leap_seconds, 18);

        // Wrong checksum, short payload: ignored
        start();
        bytes_t broken = ubx(0x01, 0x07, pvt);
        broken.back() ^= 0x01;
        feed(broken);
        feed(ubx(0x01, 0x07, bytes_t(pvt.begin(), pvt.begin() + 91)));
        CHECK(!gnss_ubx_latest_pvt(&latest, 1000000));

        // Not resolved, no fix
        feed(ubx(0x01, 0x07, nav_pvt(302418000, 2026, 10, 28, 12, 0, 0, 0x03, 2, 0x00, 4)));
        CHECK(gnss_ubx_latest_pvt(&latest, 1000000));
        CHECK_EQ(gnss_ubx_pvt_invalid_reasons(latest, 0), GNSS_UBX_INVALID_FIX | GNSS_UBX_INVALID_NOT_RESOLVED);
    }

    void test_ubx_tim_tp()
    {
        start();
        feed(ubx(0x01, 0x07, nav_pvt(302418000, 2026, 10, 28, 12, 0, 0, 0x07, 3, 0x01, 14)));
        // The pulse at 12:00:01 UTC on the GPS time base (time of week 302419 s), qErr -1234 ps
        bytes_t tp(16, 0);
        put_u4(tp, 0, 302419000);
        put_u4(tp, 8, static_cast<uint32_t>(-1234));
        feed(ubx(0x0D, 0x01, tp));
        int32_t qerr = 0;
        CHECK(gnss_ubx_qerr_for_edge(1793188801, &qerr));
        CHECK_EQ(qerr, -1234);
        CHECK(!gnss_ubx_qerr_for_edge(1793188802, &qerr)); // no TIM-TP for that pulse
        // Flagged qErrInvalid
        tp[14] = 0x10;
        put_u4(tp, 0, 302420000);
        feed(ubx(0x0D, 0x01, tp));
        CHECK(!gnss_ubx_qerr_for_edge(1793188802, &qerr));
        // UTC time base (flags bit 0): time of week without the leap seconds
        tp[14] = 0x01;
        put_u4(tp, 0, 302403000);
        put_u4(tp, 8, 777);
        feed(ubx(0x0D, 0x01, tp));
        CHECK(gnss_ubx_qerr_for_edge(1793188803, &qerr));
        CHECK_EQ(qerr, 777);
    }

    void test_ubx_commands()
    {
        // MON-VER with the extensions of an M10
        start();
        bytes_t ver(40 + 4 * 30, 0);
        const char *extensions[] = {"ROM BASE 0x118B2060", "FWVER=SPG 5.10", "PROTVER=34.10", "MOD=NEO-M10"};
        for (size_t i = 0; i < 4; ++i)
            std::memcpy(ver.data() + 40 + i * 30, extensions[i], std::strlen(extensions[i]));
        s_answer = answer_t{true, 0x0A, 0x04, ver};
        gnss_ubx_version_t version{};
        CHECK(gnss_ubx_query_version(&version, 1000));
        CHECK(std::strcmp(version.firmware_type, "SPG") == 0);
        CHECK_EQ(version.firmware_high, 5);
        CHECK_EQ(version.firmware_low, 10);
        CHECK_EQ(version.protocol_high, 34);
        CHECK_EQ(version.protocol_low, 10);
        CHECK(std::strcmp(version.module, "NEO-M10") == 0);
        CHECK(s_last_command.size() == 8 && s_last_command[2] == 0x0A && s_last_command[3] == 0x04);

        // No answer: false after the timeout (simulated time)
        s_answer = answer_t{};
        const int64_t before_us = host_now_us;
        CHECK(!gnss_ubx_query_version(&version, 500));
        CHECK(host_now_us - before_us >= 500000);

        // SEC-UNIQID version 2 (M10, 6 bytes)
        s_answer = answer_t{true, 0x27, 0x03, {2, 0, 0, 0, 0x01, 0x02, 0x03, 0x04, 0x05, 0xAB}};
        char id[16] = "";
        CHECK(gnss_ubx_query_unique_id(id, sizeof(id), 1000));
        CHECK(std::strcmp(id, "0102030405AB") == 0);
        CHECK(!gnss_ubx_query_unique_id(id, 12, 1000)); // buffer too small

        // MON-RF: jamming OK, antenna OK and powered
        bytes_t rf(4 + 24, 0);
        rf[1] = 1;
        rf[4 + 1] = 0x01;
        rf[4 + 2] = 2;
        rf[4 + 3] = 1;
        put_u2(rf, 4 + 12, 87);
        put_u2(rf, 4 + 14, 1500);
        rf[4 + 16] = 9;
        s_answer = answer_t{true, 0x0A, 0x38, rf};
        gnss_ubx_rf_t rf_status{};
        CHECK(gnss_ubx_query_rf(&rf_status, 1000));
        CHECK_EQ(rf_status.jamming_state, 1);
        CHECK_EQ(rf_status.antenna_status, 2);
        CHECK_EQ(rf_status.antenna_power, 1);
        CHECK_EQ(rf_status.noise_per_ms, 87);
        CHECK_EQ(rf_status.agc_count, 1500);
        CHECK_EQ(rf_status.jam_indicator, 9);

        // CFG-VALGET of CFG-ITFM-ENABLE (1 byte) from RAM, then a NAK and an answer for another key
        s_answer = answer_t{true, 0x06, 0x8B, {1, 0, 0, 0, 0x0d, 0x00, 0x41, 0x10, 0x01}, false, true};
        uint32_t value = 0;
        CHECK(gnss_ubx_val_get(GNSS_UBX_CFG_ITFM_ENABLE, GNSS_UBX_LAYER_RAM, &value, 1000));
        CHECK_EQ(value, 1);
        s_answer = answer_t{true, 0x06, 0x8B, {}, true};
        CHECK(!gnss_ubx_val_get(GNSS_UBX_CFG_ITFM_ENABLE, GNSS_UBX_LAYER_RAM, &value, 1000));
        s_answer = answer_t{true, 0x06, 0x8B, {1, 0, 0, 0, 0x0e, 0x00, 0x41, 0x10, 0x01}, false, true};
        CHECK(!gnss_ubx_val_get(GNSS_UBX_CFG_ITFM_ENABLE, GNSS_UBX_LAYER_RAM, &value, 1000));

        // CFG-VALSET: ACK, NAK; the frame carries the key and the 1-byte value
        s_answer = answer_t{true, 0, 0, {}, false, true};
        const gnss_ubx_cfg_item_t item{GNSS_UBX_CFG_ITFM_ENABLE, 1};
        CHECK(gnss_ubx_val_set(&item, 1, GNSS_UBX_LAYER_RAM, 1000));
        CHECK_EQ(s_last_command.size(), 8 + 4 + 5);
        CHECK_EQ(s_last_command[6 + 1], GNSS_UBX_LAYER_RAM);
        CHECK_EQ(s_last_command[6 + 8], 1);
        s_answer = answer_t{true, 0, 0, {}, true};
        CHECK(!gnss_ubx_val_set(&item, 1, GNSS_UBX_LAYER_RAM, 1000));
        gnss_ubx_status_t status{};
        gnss_ubx_get_status(&status);
        CHECK(status.naks >= 2);

        // Not running: no command is sent
        start(false);
        s_answer = answer_t{true, 0x0A, 0x04, ver};
        CHECK(!gnss_ubx_query_version(&version, 1000));
        CHECK(s_last_command.empty());
    }
}

// Seed inputs for the fuzzer (format of LLVMFuzzerTestOneInput): one file per kind of message
void write_seeds(const char *directory)
{
    const bytes_t pvt = nav_pvt(302418000, 2026, 10, 28, 12, 0, 0, 0x07, 3, 0x01, 14);
    bytes_t tp(16, 0);
    put_u4(tp, 0, 302419000);
    bytes_t ver(40 + 3 * 30, 0);
    const char *extensions[] = {"FWVER=SPG 5.10", "PROTVER=34.10", "MOD=NEO-M10"};
    for (size_t i = 0; i < 3; ++i)
        std::memcpy(ver.data() + 40 + i * 30, extensions[i], std::strlen(extensions[i]));
    const std::string streams[] = {
        nmea(RMC_VALID),
        nmea("GNGGA,123519.00,4807.03800,N,01131.00000,E,1,12,0.9,545.4,M,46.9,M,,") + nmea(RMC_VALID),
        gsv("GP", 2, 1, 6, "1") + gsv("GP", 2, 2, 6, "1"),
    };
    const bytes_t frames[] = {ubx(0x01, 0x07, pvt), ubx(0x0D, 0x01, tp), ubx(0x01, 0x21, bytes_t(20, 0x04))};
    std::vector<bytes_t> seeds;
    for (const std::string &stream : streams)
    {
        bytes_t seed = {static_cast<uint8_t>((stream.size() + 4) / 4)};
        seed.insert(seed.end(), stream.begin(), stream.end());
        seeds.push_back(seed);
    }
    for (const bytes_t &frame : frames)
    {
        bytes_t seed = {static_cast<uint8_t>((frame.size() + 4) / 4)};
        seed.insert(seed.end(), frame.begin(), frame.end());
        seeds.push_back(seed);
    }
    bytes_t answer = {1, 0x20, 0x20, 0x20}; // three stream bytes, then the answer
    answer.insert(answer.end(), ver.begin(), ver.end());
    seeds.push_back(answer);
    for (size_t i = 0; i < seeds.size(); ++i)
    {
        const std::string path = std::string(directory) + "/seed_" + std::to_string(i);
        FILE *file = std::fopen(path.c_str(), "wb");
        if (file == nullptr)
            continue;
        std::fwrite(seeds[i].data(), 1, seeds[i].size(), file);
        std::fclose(file);
    }
}

#ifndef HOST_FUZZ
int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--seeds") == 0)
    {
        write_seeds(argv[2]);
        return 0;
    }
    test_nmea_checksum();
    test_nmea_rmc_parse();
    test_uart_nmea_framing();
    test_nmea_gga_and_min_satellites();
    test_nmea_gsv();
    test_nmea_rmc_phase();
    test_ubx_nav_pvt();
    test_ubx_tim_tp();
    test_ubx_commands();
    std::printf("gnss_test: %d checks, %d failed\n", host_checks, host_failures);
    return host_failures == 0 ? 0 : 1;
}
#else
// Fuzzing: the input is a byte stream from the receiver (with flushes), and the answer to the commands
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    start();
    if (size < 2)
        return 0;
    const size_t split = std::min<size_t>(size, data[0] * 4u);
    // Stream: a byte 0xFE starts a new flush generation (as gnss_uart_flush())
    size_t begin = 1;
    for (size_t i = 1; i < split; ++i)
    {
        if (data[i] == 0xFE)
        {
            host_gnss_uart_feed(data + begin, i - begin, s_generation);
            ++s_generation;
            host_now_us += 1000;
            begin = i + 1;
        }
    }
    if (split > begin)
        host_gnss_uart_feed(data + begin, split - begin, s_generation);

    // What the parsers kept must be plausible
    nmea_rmc_time_t t{};
    if (gnss_nmea_wait_valid_rmc(1, host_now_us, &t) &&
        (t.year < 2026 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 60))
        __builtin_trap();
    gnss_ubx_pvt_t pvt{};
    (void)gnss_ubx_latest_pvt(&pvt, 2000000);
    int32_t qerr = 0;
    (void)gnss_ubx_qerr_for_edge(1793188801, &qerr);
    gnss_nmea_status_t nmea_status{};
    gnss_nmea_get_status(&nmea_status);

    // The rest is the answer of the receiver to each command
    s_answer = answer_t{true, 0, 0, bytes_t(data + split, data + size), false, true};
    gnss_ubx_version_t version{};
    s_answer.cls = 0x0A, s_answer.id = 0x04;
    (void)gnss_ubx_query_version(&version, 10);
    if (std::strlen(version.firmware_type) >= sizeof(version.firmware_type) || std::strlen(version.module) >= sizeof(version.module))
        __builtin_trap();
    char id[13];
    s_answer.cls = 0x27, s_answer.id = 0x03;
    if (gnss_ubx_query_unique_id(id, sizeof(id), 10) && std::strlen(id) > 12)
        __builtin_trap();
    gnss_ubx_rf_t rf{};
    s_answer.cls = 0x0A, s_answer.id = 0x38;
    (void)gnss_ubx_query_rf(&rf, 10);
    uint32_t value = 0;
    s_answer.cls = 0x06, s_answer.id = 0x8B;
    (void)gnss_ubx_val_get(GNSS_UBX_CFG_ITFM_ENABLE, GNSS_UBX_LAYER_RAM, &value, 10);
    return 0;
}
#endif
