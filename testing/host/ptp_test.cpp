// ESP32 Time Server - host tests of the PTP protocol code (see README.md)
// Upstream testing/firmware_regression/ptp_host_tests.cpp (release 4.0) adapted to the fork's parser,
// plus the fork's tests of ptp_message.h and ptp_l3_udp.h. Built twice by run_tests.ps1: as a test program (main) and,
// with HOST_FUZZ, as a libFuzzer target (the input is a received Ethernet frame).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define private public
#include "ptp_followers.h"
#undef private
#include "ptp_l2_protocol.h"
#include "ptp_l3_udp.h"
#include "ptp_message.h"

namespace
{
    int failures = 0;

    void check(bool condition, const char *expression, int line)
    {
        if (!condition)
        {
            std::fprintf(stderr, "Failure at line %d: %s\n", line, expression);
            ++failures;
        }
    }

#define CHECK(expression) check((expression), #expression, __LINE__)

    using Frame = std::vector<uint8_t>;

    void write_u16(uint8_t *data, uint16_t value)
    {
        data[0] = static_cast<uint8_t>(value >> 8);
        data[1] = static_cast<uint8_t>(value);
    }

    void write_u32(uint8_t *data, uint32_t value)
    {
        data[0] = static_cast<uint8_t>(value >> 24);
        data[1] = static_cast<uint8_t>(value >> 16);
        data[2] = static_cast<uint8_t>(value >> 8);
        data[3] = static_cast<uint8_t>(value);
    }

    Frame make_delay_request()
    {
        Frame frame(60, 0);
        const uint8_t destination[6] = {0x01, 0x1b, 0x19, 0x00, 0x00, 0x00};
        const uint8_t source[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
        std::memcpy(frame.data(), destination, sizeof(destination));
        std::memcpy(frame.data() + 6, source, sizeof(source));
        frame[12] = 0x88;
        frame[13] = 0xf7;
        uint8_t *message = frame.data() + 14;
        message[0] = 1;
        message[1] = 2;
        write_u16(message + 2, 44);
        message[4] = 7;
        for (size_t i = 0; i < 8; ++i)
            message[20 + i] = static_cast<uint8_t>(i + 1);
        message[28] = 0x12;
        message[29] = 0x34;
        write_u16(message + 30, 0x4567);
        message[32] = 1;
        write_u32(message + 40, 999999999U);
        return frame;
    }

    Frame make_announce()
    {
        Frame frame(78, 0);
        const Frame delay = make_delay_request();
        std::memcpy(frame.data(), delay.data(), 48);
        uint8_t *message = frame.data() + 14;
        message[0] = 11;
        write_u16(message + 2, 64);
        message[32] = 5;
        message[33] = 1;
        message[47] = 128;
        message[48] = 6;
        message[49] = 0x20;
        write_u16(message + 50, 0x1234);
        message[52] = 129;
        for (size_t i = 0; i < 8; ++i)
            message[53 + i] = static_cast<uint8_t>(0xa0 + i);
        write_u16(message + 61, 254);
        message[63] = 0x20;
        return frame;
    }

    ptp::MessageKind parse(const Frame &frame, size_t length, uint8_t domain = 7,
                           const uint8_t *local_identity = nullptr)
    {
        ptp::ParsedMessage parsed{};
        return ptp::parse_l2_frame(frame.data(), length, domain, local_identity, parsed);
    }

    void test_follower_lifecycle()
    {
        const uint8_t first[10] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 1};
        const uint8_t other_port[10] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 2};
        ptp::FollowerTable<3> table(100);
        CHECK(table.observe(1, first, 1000, first));
        CHECK(table.observe(1, first, 1050, first));
        CHECK(table.snapshot_and_reset(1050).current == 1);
        CHECK(table.observe(2, first, 1060, first));
        CHECK(table.observe(1, other_port, 1070, other_port));
        ptp::Snapshot current = table.snapshot_and_reset(1070);
        CHECK(current.current == 3);
        CHECK(current.min == 1);
        CHECK(current.max == 3);
        CHECK(table.snapshot_and_reset(1070).min == 3);
        CHECK(table.snapshot_and_reset(1070).max == 3);
        table.expire(1169);
        CHECK(table.snapshot_and_reset(1169).current == 1);
        table.expire(1170);
        CHECK(table.snapshot_and_reset(1170).current == 0);
    }

    void test_follower_rollover_snapshots_clear_and_capacity()
    {
        const uint8_t identity[10] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
        ptp::FollowerTable<1> signed_range_table(100);
        CHECK(signed_range_table.observe(0, identity, -50, identity));
        signed_range_table.expire(49);
        CHECK(signed_range_table.snapshot_and_reset(49).current == 1);
        signed_range_table.expire(50);
        CHECK(signed_range_table.snapshot_and_reset(50).current == 0);

        ptp::FollowerTable<2> table(1000);
        CHECK(table.observe(0, identity, 0, identity));
        const uint8_t second[10] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 1};
        CHECK(table.observe(0, second, 1, second));
        CHECK(table.observe(0, identity, 2, identity));
        const ptp::Snapshot frozen = table.snapshot_and_reset(2);
        CHECK(frozen.current == 2);
        table.clear();
        CHECK(table.snapshot_and_reset(2).current == 0);
        CHECK(frozen.current == 2);
        CHECK(table.observe(0, identity, 3, identity));
        CHECK(table.observe(0, second, 3, second));
        CHECK(!table.observe(1, second, 3, second));
        CHECK(table.snapshot_and_reset(3).overflow_drops == 1);

        ptp::FollowerTable<0> zero_capacity(100);
        CHECK(!zero_capacity.observe(0, identity, 0, identity));
        CHECK(zero_capacity.snapshot_and_reset(0).overflow_drops == 1);
        CHECK(!zero_capacity.observe(0, identity, 1, identity));
        CHECK(zero_capacity.snapshot_and_reset(1).overflow_drops == 1);

        ptp::FollowerTable<0> saturation(100);
        saturation.snapshot_.overflow_drops = std::numeric_limits<uint32_t>::max() - 1;
        CHECK(!saturation.observe(0, identity, 0, identity));
        CHECK(saturation.snapshot_.overflow_drops == std::numeric_limits<uint32_t>::max());
        CHECK(!saturation.observe(0, identity, 1, identity));
        CHECK(saturation.snapshot_.overflow_drops == std::numeric_limits<uint32_t>::max());
    }

    void test_follower_report_serialization()
    {
        char report[ptp::FOLLOWER_REPORT_CAPACITY]{};
        const ptp::Snapshot snapshot{2, 0, 3, 5};
        const int size = ptp::format_follower_report(report, sizeof(report), snapshot);
        CHECK(size > 0 && static_cast<size_t>(size) < sizeof(report));
        CHECK(std::string(report) == "\"ptp\":{\"followers\":{\"current\":2,\"min\":0,\"max\":3},\"clients\":[],\"follower_overflow_drops\":5}");
        const std::string frozen = report;
        const std::string combined = "{\"this_period\":{\"ntp\":{}," + frozen + "}}";
        CHECK(combined.find("\"ntp\":{},\"ptp\":") != std::string::npos);
        const std::string ptp_only = "{\"this_period\":{" + frozen + "}}";
        CHECK(ptp_only.find("\"ntp\"") == std::string::npos);
        ptp::FollowerTable<1> table(100);
        table.clear();
        CHECK(frozen == report);
        const auto maximum = std::numeric_limits<uint32_t>::max();
        const ptp::Snapshot worst_case{maximum, maximum, maximum, maximum};
        CHECK(ptp::format_follower_report(report, sizeof(report), worst_case) < static_cast<int>(sizeof(report)));
        char short_buffer[4]{};
        CHECK(ptp::format_follower_report(short_buffer, sizeof(short_buffer), snapshot) >= static_cast<int>(sizeof(short_buffer)));
        CHECK(short_buffer[3] == '\0');
    }

    void test_client_continuity_and_deduplication()
    {
        const uint8_t first[10] = {1};
        const uint8_t second[10] = {2};
        const uint8_t mac[6] = {0, 0xf1, 0xb2, 0xd2, 0xca, 0x9a};
        ptp::FollowerTable<2> table(100);
        CHECK(table.observe(0, first, 0, mac));
        CHECK(table.observe(0, second, 50, mac));
        auto snapshot = table.snapshot_and_reset(50);
        CHECK(snapshot.current == 2 && snapshot.client_count == 1);
        CHECK(!snapshot.clients[0].active_entire_period);
        table.expire(100);
        snapshot = table.snapshot_and_reset(100);
        CHECK(snapshot.current == 1 && snapshot.client_count == 1);
        CHECK(snapshot.clients[0].active_entire_period);
        table.expire(150);
        CHECK(table.observe(0, first, 151, mac));
        snapshot = table.snapshot_and_reset(151);
        CHECK(snapshot.client_count == 1 && !snapshot.clients[0].active_entire_period);
        CHECK(table.observe(0, first, 152, mac));
        CHECK(table.snapshot_and_reset(152).clients[0].active_entire_period);
        table.clear();
        CHECK(table.observe(0, first, 153, mac));
        snapshot = table.snapshot_and_reset(153);
        CHECK(snapshot.client_count == 1 && !snapshot.clients[0].active_entire_period);
        CHECK(std::memcmp(snapshot.clients[0].mac, mac, sizeof(mac)) == 0);
        char report[ptp::FOLLOWER_REPORT_CAPACITY]{};
        CHECK(ptp::format_follower_report(report, sizeof(report), snapshot) > 0);
        CHECK(std::string(report).find("\"mac\":\"00:f1:b2:d2:ca:9a\",\"active_entire_period\":false") != std::string::npos);
    }

    void test_client_report_limit_and_first_arrival_order()
    {
        ptp::FollowerTable<32> table(100);
        for (uint8_t i = 0; i < 32; ++i)
        {
            const uint8_t identity[10] = {i};
            CHECK(table.observe(0, identity, 0, identity));
        }
        CHECK(table.snapshot_and_reset(0).client_count == 32);
        auto snapshot = table.snapshot_and_reset(0);
        for (size_t i = 0; i < 32; ++i)
            CHECK(snapshot.clients[i].active_entire_period);
        int64_t now = 0;
        for (uint8_t i = 32; i < 150; ++i)
        {
            now += 50;
            for (uint8_t j = 0; j < 31; ++j)
            {
                const uint8_t identity[10] = {j};
                CHECK(table.observe(0, identity, now, identity));
            }
            const uint8_t identity[10] = {31};
            const uint8_t mac[6] = {i};
            CHECK(table.observe(0, identity, now, mac));
        }
        snapshot = table.snapshot_and_reset(now);
        constexpr size_t limit = ptp::DEFAULT_CLIENT_REPORT_LIMIT;
        CHECK(snapshot.client_count == limit);
        for (size_t i = 0; i < limit; ++i)
        {
            CHECK(snapshot.clients[i].mac[0] == i);
            CHECK(snapshot.clients[i].active_entire_period == (i < 31));
        }
        CHECK(table.snapshot_and_reset(now).client_count == 32);

        ptp::FollowerTable<2, 3> limited(100);
        for (uint8_t i = 0; i < 5; ++i)
        {
            const uint8_t identity[10] = {static_cast<uint8_t>(i % 2)};
            const uint8_t mac[6] = {i};
            (void)limited.observe(0, identity, i, mac);
        }
        const auto limited_snapshot = limited.snapshot_and_reset(5);
        CHECK(limited_snapshot.client_count == 3 && limited_snapshot.clients.size() == 3);

        snapshot.current = snapshot.min = snapshot.max = snapshot.overflow_drops = UINT32_MAX;
        for (auto &client : snapshot.clients)
        {
            std::memset(client.mac, 0xff, sizeof(client.mac));
            client.active_entire_period = false;
        }
        char report[ptp::FOLLOWER_REPORT_CAPACITY]{};
        const int size = ptp::format_follower_report(report, sizeof(report), snapshot);
        CHECK(size > 0 && static_cast<size_t>(size) < sizeof(report));
        CHECK(static_cast<size_t>(size) < ptp::follower_report_capacity(snapshot.client_count));
        std::vector<char> exact(static_cast<size_t>(size) + 1);
        CHECK(ptp::format_follower_report(exact.data(), exact.size(), snapshot) == size);
        CHECK(ptp::format_follower_report(exact.data(), exact.size() - 1, snapshot) == size);
        CHECK(ptp::format_follower_report(nullptr, 0, snapshot) == size);
    }

    void test_frozen_report_periods()
    {
        const uint8_t identity[10] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 1};
        ptp::FollowerTable<1> table(100);
        std::vector<std::string> queued;
        char report[ptp::FOLLOWER_REPORT_CAPACITY]{};
        const auto generate = [&](int64_t now_us)
        {
            const auto snapshot = table.snapshot_and_reset(now_us);
            CHECK(snapshot.min <= snapshot.current && snapshot.current <= snapshot.max);
            CHECK(ptp::format_follower_report(report, sizeof(report), snapshot) > 0);
            queued.emplace_back(report);
        };
        generate(0);
        CHECK(table.observe(0, identity, 1, identity));
        generate(2);
        CHECK(table.observe(0, identity, 3, identity));
        generate(4);
        table.expire(103);
        generate(104);
        CHECK(queued[0].find("\"current\":0,\"min\":0,\"max\":0") != std::string::npos);
        CHECK(queued[1].find("\"current\":1,\"min\":0,\"max\":1") != std::string::npos);
        CHECK(queued[2].find("\"current\":1,\"min\":1,\"max\":1") != std::string::npos);
        CHECK(queued[3].find("\"current\":0,\"min\":0,\"max\":1") != std::string::npos);
        FILE *storage = nullptr;
#if defined(_MSC_VER)
        CHECK(tmpfile_s(&storage) == 0);
#else
        storage = std::tmpfile();
#endif
        CHECK(storage != nullptr);
        if (storage != nullptr)
        {
            const std::string frozen = queued[1];
            CHECK(std::fwrite(frozen.data(), 1, frozen.size(), storage) == frozen.size());
            table.clear();
            generate(105);
            std::rewind(storage);
            char restored[ptp::FOLLOWER_REPORT_CAPACITY]{};
            CHECK(std::fread(restored, 1, frozen.size(), storage) == frozen.size());
            CHECK(std::string(restored) == frozen);
            CHECK(queued[1] == frozen);
            std::fclose(storage);
        }
        const auto after_retry = table.snapshot_and_reset(106);
        CHECK(after_retry.current == 0 && after_retry.min == 0 && after_retry.max == 0);
    }

    void test_delay_request_validity_and_output()
    {
        Frame frame = make_delay_request();
        ptp::ParsedMessage parsed{};
        CHECK(ptp::parse_l2_frame(frame.data(), frame.size(), 7, nullptr, parsed) == ptp::MessageKind::DelayRequest);
        CHECK(parsed.domain == 7);
        CHECK(parsed.sequence_id == 0x4567);
        CHECK(std::memcmp(parsed.source_port_identity, frame.data() + 34, 10) == 0);
        write_u32(frame.data() + 14 + 40, 999999999U);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::DelayRequest);
        write_u32(frame.data() + 14 + 40, 1000000000U);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
    }

    void test_frame_ethernet_and_dispatch_validation()
    {
        Frame frame = make_delay_request();
        ptp::ParsedMessage parsed{};
        CHECK(ptp::parse_l2_frame(nullptr, 60, 7, nullptr, parsed) == ptp::MessageKind::Invalid);
        CHECK(parse(frame, 13) == ptp::MessageKind::Invalid);
        CHECK(parse(frame, 14) == ptp::MessageKind::Invalid);
        CHECK(parse(frame, 47) == ptp::MessageKind::Invalid);
        CHECK(parse(frame, 57) == ptp::MessageKind::Invalid);
        CHECK(parse(frame, 58) == ptp::MessageKind::DelayRequest);
        CHECK(parse(frame, 59) == ptp::MessageKind::DelayRequest);
        CHECK(parse(frame, 60) == ptp::MessageKind::DelayRequest);
        CHECK(parse(frame, 61) == ptp::MessageKind::Invalid);

        frame[12] = 0x08;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Ignored);
        frame = make_delay_request();
        frame[0] = 0x02;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Ignored);
        frame = make_delay_request();
        frame[6] = 0x01;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        std::memset(frame.data() + 6, 0, 6);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        frame[14] = 2;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Ignored);
        frame = make_delay_request();
        CHECK(parse(frame, frame.size(), 8) == ptp::MessageKind::Ignored);
        const uint8_t local_identity[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(parse(frame, frame.size(), 7, local_identity) == ptp::MessageKind::Ignored);
    }

    void test_delay_request_fields()
    {
        Frame frame = make_delay_request();
        uint8_t *message = frame.data() + 14;
        write_u16(message + 2, 43);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        write_u16(message + 2, 45);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);

        frame = make_delay_request();
        message = frame.data() + 14;
        message[0] |= 0x10;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        message = frame.data() + 14;
        message[1] = 0x22;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        message = frame.data() + 14;
        message[1] = 0x03;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        message = frame.data() + 14;
        message[5] = 1;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);

        // Fork: the correctionField of a Delay_Req is accepted and returned (IEEE 1588-2019 11.3.2)
        for (size_t i = 8; i < 16; ++i)
        {
            frame = make_delay_request();
            frame[14 + i] = 1;
            ptp::ParsedMessage parsed{};
            CHECK(ptp::parse_l2_frame(frame.data(), frame.size(), 7, nullptr, parsed) == ptp::MessageKind::DelayRequest);
            CHECK(parsed.correction == static_cast<int64_t>(uint64_t{1} << (8 * (15 - i))));
        }
        // Fork: the flagField of a Delay_Req is not checked (unicastFlag and profile flags are legal)
        frame = make_delay_request();
        frame[14 + 6] = 0x04;
        frame[14 + 7] = 0x08;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::DelayRequest);
        for (size_t i = 16; i < 20; ++i)
        {
            frame = make_delay_request();
            frame[14 + i] = 1;
            CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        }
        frame = make_delay_request();
        frame[14 + 28] = 0;
        frame[14 + 29] = 0;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        frame[14 + 28] = 0xff;
        frame[14 + 29] = 0xff;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        std::memset(frame.data() + 14 + 20, 0, 8);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        std::memset(frame.data() + 14 + 20, 0xff, 8);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_delay_request();
        frame[14 + 32] = 0;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
    }

    void test_version_and_control_compatibility()
    {
        for (const uint8_t type : {uint8_t{1}, uint8_t{11}})
        {
            const uint8_t legacy_control = type == 1 ? 1 : 5;
            const auto expected = type == 1 ? ptp::MessageKind::DelayRequest : ptp::MessageKind::Announce;
            for (const bool allow_authentication : {false, true})
            {
                for (const uint8_t version : {uint8_t{2}, uint8_t{0x12}})
                {
                    for (const uint8_t control : {uint8_t{0}, uint8_t{1}, uint8_t{5}, uint8_t{0xff}})
                    {
                        Frame frame = type == 1 ? make_delay_request() : make_announce();
                        frame[15] = version;
                        frame[14 + 32] = control;
                        ptp::ParsedMessage parsed{};
                        const bool compatible = version == 0x12 || control == legacy_control;
                        CHECK(ptp::parse_l2_frame(frame.data(), frame.size(), 7, nullptr, parsed,
                                                  allow_authentication) ==
                              (compatible ? expected : ptp::MessageKind::Invalid));
                        if (type == 1)
                            CHECK(ptp::parse_l2_frame(frame.data(), 58, 7, nullptr, parsed,
                                                      allow_authentication) ==
                                  (compatible ? expected : ptp::MessageKind::Invalid));
                    }
                }
            }

            Frame frame = type == 1 ? make_delay_request() : make_announce();
            const size_t body = type == 1 ? 44 : 64;
            frame.resize(14 + body + ptp::AUTH_TLV_SIZE);
            frame[15] = 0x12;
            frame[14 + 32] = 0;
            write_u16(frame.data() + 16, static_cast<uint16_t>(body + ptp::AUTH_TLV_SIZE));
            ptp::ParsedMessage parsed{};
            CHECK(ptp::parse_l2_frame(frame.data(), frame.size(), 7, nullptr, parsed, true) == expected);
            CHECK(ptp::parse_l2_frame(frame.data(), frame.size(), 7, nullptr, parsed, false) ==
                  ptp::MessageKind::Invalid);
        }
    }

    void test_announce_validation()
    {
        Frame frame = make_announce();
        ptp::ParsedMessage parsed{};
        CHECK(ptp::parse_l2_frame(frame.data(), frame.size(), 7, nullptr, parsed) == ptp::MessageKind::Announce);
        CHECK(parsed.announce_dataset.priority1 == 128 && parsed.announce_dataset.clock_class == 6);
        CHECK(parsed.announce_dataset.clock_accuracy == 0x20 && parsed.announce_dataset.variance == 0x1234);
        CHECK(parsed.announce_dataset.priority2 == 129 && parsed.announce_dataset.identity[0] == 0xa0);
        CHECK(parsed.steps_removed == 254 && parsed.log_announce_interval == 1);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Announce);
        CHECK(parse(frame, frame.size() - 1) == ptp::MessageKind::Invalid);
        frame.push_back(0);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);

        frame = make_announce();
        uint8_t *message = frame.data() + 14;
        frame[14] |= 0x10;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        message[6] = 0x02;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        message[7] = 0x40;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        message[7] = 0x03;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        message[32] = 4;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        message[46] = 1;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        write_u16(message + 61, 255);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        std::memset(message + 53, 0, 8);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        std::memset(message + 53, 0xff, 8);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        message[48] = 0;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        message[48] = 0xff;
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
        frame = make_announce();
        message = frame.data() + 14;
        write_u32(message + 40, 1000000000U);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);

        frame = make_announce();
        message = frame.data() + 14;
        write_u16(message + 2, 65);
        frame.resize(79, 0);
        CHECK(parse(frame, frame.size()) == ptp::MessageKind::Invalid);
    }

    uint32_t next_random(uint32_t &state)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    void test_deterministic_parser_fuzz()
    {
        uint32_t state = 0x7a39d4b1U;
        Frame frame(128);
        ptp::ParsedMessage parsed{};
        for (size_t iteration = 0; iteration < 30000; ++iteration)
        {
            const size_t actual_size = next_random(state) % (frame.size() + 1);
            for (uint8_t &byte : frame)
                byte = static_cast<uint8_t>(next_random(state));
            if (actual_size >= 14 && (iteration & 1U) == 0)
            {
                const Frame seed = (iteration & 2U) == 0 ? make_delay_request() : make_announce();
                std::memcpy(frame.data(), seed.data(), seed.size());
                frame[14 + (next_random(state) % 64)] ^= static_cast<uint8_t>(next_random(state));
                frame[12] = 0x88;
                frame[13] = 0xf7;
                std::memcpy(frame.data(), seed.data(), 6);
            }
            const size_t length = next_random(state) % (actual_size + 1);
            const ptp::MessageKind result = ptp::parse_l2_frame(frame.data(), length, 7, nullptr, parsed);
            CHECK(result == ptp::MessageKind::Ignored || result == ptp::MessageKind::Invalid ||
                  result == ptp::MessageKind::DelayRequest || result == ptp::MessageKind::Announce);
        }
    }

    // ---- Fork: ptp_message.h ----

    ptp::PortConfig test_port()
    {
        ptp::PortConfig port{};
        port.domain = 7;
        const uint8_t mac[6] = {0x80, 0xf1, 0xb2, 0xd6, 0x58, 0x1d};
        ptp::clock_identity_from_mac(mac, port.identity);
        port.port_number = 1;
        port.log_sync_interval = 0;
        port.log_announce_interval = 1;
        port.log_min_delay_req_interval = -1;
        port.dataset.priority1 = 100;
        port.dataset.priority2 = 200;
        port.dataset.clock_class = ptp::CLOCK_CLASS_LOCKED;
        port.dataset.clock_accuracy = ptp::CLOCK_ACCURACY_250NS;
        port.dataset.variance = ptp::VARIANCE_GNSS;
        port.utc_offset = 37;
        port.utc_offset_valid = true;
        port.time_traceable = true;
        port.frequency_traceable = true;
        return port;
    }

    uint64_t read_seconds48(const uint8_t *data)
    {
        uint64_t value = 0;
        for (size_t i = 0; i < 6; ++i)
            value = (value << 8) | data[i];
        return value;
    }

    uint32_t read_u32(const uint8_t *data)
    {
        return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) | data[3];
    }

    void test_message_builder()
    {
        const ptp::PortConfig port = test_port();
        const uint8_t expected_identity[8] = {0x80, 0xf1, 0xb2, 0xff, 0xfe, 0xd6, 0x58, 0x1d};
        CHECK(std::memcmp(port.identity, expected_identity, 8) == 0);

        // 2026-11-08 12:00:00 UTC + 37 s, plus 123456789 ns
        const int64_t tai_ns = (1794139200LL + 37) * 1000000000LL + 123456789LL;
        uint8_t message[80];

        std::memset(message, 0xaa, sizeof(message));
        CHECK(ptp::build_sync(message, sizeof(message), port, 0x1234, tai_ns) == ptp::SYNC_SIZE);
        CHECK(message[0] == 0 && message[1] == 2 && ptp::detail::read_u16(message + 2) == 44 && message[4] == 7);
        CHECK(message[6] == ptp::FLAG_TWO_STEP && message[7] == 0);
        CHECK(ptp::detail::all_bytes_equal(message + 8, 12, 0));
        CHECK(std::memcmp(message + 20, expected_identity, 8) == 0 && ptp::detail::read_u16(message + 28) == 1);
        CHECK(ptp::detail::read_u16(message + 30) == 0x1234 && message[32] == 0 && message[33] == 0);
        CHECK(read_seconds48(message + 34) == 1794139237ULL && read_u32(message + 40) == 123456789U);
        CHECK(message[44] == 0xaa); // nothing written after the message

        CHECK(ptp::build_follow_up(message, sizeof(message), port, 0x1234, tai_ns + 5) == ptp::FOLLOW_UP_SIZE);
        CHECK(message[0] == 8 && message[6] == 0 && message[32] == 2 && message[33] == 0);
        CHECK(read_u32(message + 40) == 123456794U);

        CHECK(ptp::build_announce(message, sizeof(message), port, 9, 0) == ptp::ANNOUNCE_SIZE);
        CHECK(message[0] == 11 && ptp::detail::read_u16(message + 2) == 64 && message[32] == 5 && message[33] == 1);
        CHECK(message[6] == 0);
        CHECK(message[7] == (ptp::FLAG_PTP_TIMESCALE | ptp::FLAG_UTC_OFFSET_VALID | ptp::FLAG_TIME_TRACEABLE |
                             ptp::FLAG_FREQUENCY_TRACEABLE));
        CHECK(ptp::detail::all_bytes_equal(message + 34, 10, 0));
        CHECK(ptp::detail::read_u16(message + 44) == 37 && message[46] == 0);
        CHECK(message[47] == 100 && message[48] == 6 && message[49] == 0x22 &&
              ptp::detail::read_u16(message + 50) == 0x4e5d && message[52] == 200);
        CHECK(std::memcmp(message + 53, expected_identity, 8) == 0);
        CHECK(ptp::detail::read_u16(message + 61) == 0 && message[63] == 0x20);

        // The own Announce passes the parser (as another master would see it) with the same dataset
        ptp::ParsedMessage parsed{};
        CHECK(ptp::parse_message(message, 64, 7, nullptr, parsed) == ptp::MessageKind::Announce);
        CHECK(parsed.announce_dataset.clock_class == 6 && parsed.announce_dataset.priority1 == 100 &&
              parsed.log_announce_interval == 1 && parsed.steps_removed == 0);
        CHECK(ptp::compare_master_datasets(parsed.announce_dataset, ptp::local_dataset(port)) == 0);
        // ... and is ignored as our own
        CHECK(ptp::parse_message(message, 64, 7, port.identity, parsed) == ptp::MessageKind::Ignored);

        // Leap flags: leap61 wins over leap59, utc offset not valid
        ptp::PortConfig leap = port;
        leap.leap61 = true;
        leap.leap59 = true;
        leap.utc_offset_valid = false;
        leap.time_traceable = false;
        leap.frequency_traceable = false;
        CHECK(ptp::build_announce(message, sizeof(message), leap, 10, tai_ns) == ptp::ANNOUNCE_SIZE);
        CHECK(message[7] == (ptp::FLAG_PTP_TIMESCALE | ptp::FLAG_LEAP61));
        CHECK(read_seconds48(message + 34) == 1794139237ULL);

        // Authenticated: minorVersionPTP 1
        ptp::PortConfig authenticated = port;
        authenticated.authenticated = true;
        CHECK(ptp::build_sync(message, sizeof(message), authenticated, 1, tai_ns) == ptp::SYNC_SIZE && message[1] == 0x12);

        // Capacity and time limits
        CHECK(ptp::build_sync(message, 43, port, 1, tai_ns) == 0);
        CHECK(ptp::build_delay_resp(message, 53, port, 1, 0, expected_identity, tai_ns) == 0);
        CHECK(ptp::build_announce(message, 63, port, 1, 0) == 0);
        CHECK(ptp::build_sync(message, sizeof(message), port, 1, -1) == 0);
        uint8_t stamp[10];
        CHECK(ptp::write_timestamp(stamp, 0) && ptp::detail::all_bytes_equal(stamp, 10, 0));
        // The largest int64 time (year 2262) fits into 48 bit seconds
        CHECK(ptp::write_timestamp(stamp, std::numeric_limits<int64_t>::max()) &&
              read_seconds48(stamp) == 9223372036ULL && read_u32(stamp + 6) == 854775807U);
        CHECK(!ptp::write_timestamp(stamp, -5) && ptp::detail::all_bytes_equal(stamp, 10, 0));
    }

    void test_delay_resp_round_trip()
    {
        const ptp::PortConfig port = test_port();
        Frame frame = make_delay_request();
        // correctionField 0x0000000000018000 = 1.5 ns
        frame[14 + 13] = 0x01;
        frame[14 + 14] = 0x80;
        ptp::ParsedMessage request{};
        CHECK(ptp::parse_l2_frame(frame.data(), frame.size(), 7, port.identity, request) == ptp::MessageKind::DelayRequest);
        CHECK(request.correction == 0x18000);

        const int64_t t4 = 1794139237LL * 1000000000LL + 999999999LL;
        uint8_t message[64];
        CHECK(ptp::build_delay_resp(message, sizeof(message), port, request.sequence_id, request.correction,
                                    request.source_port_identity, t4) == ptp::DELAY_RESP_SIZE);
        CHECK(message[0] == 9 && ptp::detail::read_u16(message + 2) == 54 && message[4] == 7);
        CHECK(message[32] == 3 && static_cast<int8_t>(message[33]) == -1);
        CHECK(ptp::detail::read_i64(message + 8) == 0x18000);
        CHECK(ptp::detail::read_u16(message + 30) == request.sequence_id);
        CHECK(read_seconds48(message + 34) == 1794139237ULL && read_u32(message + 40) == 999999999U);
        CHECK(std::memcmp(message + 44, request.source_port_identity, 10) == 0);

        // Negative correction (two's complement) is copied unchanged
        CHECK(ptp::build_delay_resp(message, sizeof(message), port, 1, -65536, request.source_port_identity, t4) ==
              ptp::DELAY_RESP_SIZE);
        CHECK(ptp::detail::read_i64(message + 8) == -65536);
    }

    // ---- Fork: ptp_l3_udp.h ----

    void test_l3_udp_round_trip()
    {
        const ptp::PortConfig port = test_port();
        const uint8_t mac[6] = {0x80, 0xf1, 0xb2, 0xd6, 0x58, 0x1d};
        const uint8_t ipv4[4] = {192, 168, 8, 123};
        const uint8_t ipv6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x82, 0xf1, 0xb2, 0xff, 0xfe, 0xd6, 0x58, 0x1d};
        uint8_t message[64];
        uint8_t frame[ptp_l3_udp::MAX_FRAME_SIZE];

        for (const bool v6 : {false, true})
        {
            // Our Announce, encapsulated, then decoded by a "foreign" clock (other identity)
            CHECK(ptp::build_announce(message, sizeof(message), port, 3, 0) == 64);
            const size_t length = ptp_l3_udp::encapsulate(frame, sizeof(frame), mac, v6 ? ipv6 : ipv4, v6, message, 64);
            CHECK(length == 14 + (v6 ? 40U : 20U) + 8 + 64);
            CHECK(std::memcmp(frame, v6 ? ptp_l3_udp::IPV6_MAC : ptp_l3_udp::IPV4_MAC, 6) == 0);
            CHECK(frame[14 + (v6 ? 7 : 8)] == 1); // TTL / hop limit 1
            ptp_l3_udp::Datagram datagram{};
            ptp::ParsedMessage parsed{};
            CHECK(ptp_l3_udp::decode(frame, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Announce);
            CHECK(datagram.ipv6 == v6 && datagram.port == ptp_l3_udp::GENERAL_PORT && datagram.length == 64);
            CHECK(std::memcmp(datagram.message, message, 64) == 0);

            // A Delay_Req goes to the event port
            Frame request = make_delay_request();
            CHECK(ptp_l3_udp::encapsulate(frame, sizeof(frame), mac, v6 ? ipv6 : ipv4, v6, request.data() + 14, 44) ==
                  14 + (v6 ? 40U : 20U) + 8 + 44);
            const size_t request_length = 14 + (v6 ? 40U : 20U) + 8 + 44;
            CHECK(ptp::detail::read_u16(frame + 14 + (v6 ? 40 : 20) + 2) == ptp_l3_udp::EVENT_PORT);
            CHECK(ptp_l3_udp::decode(frame, request_length, 7, nullptr, datagram, parsed) == ptp::MessageKind::DelayRequest);
            CHECK(parsed.sequence_id == 0x4567 && datagram.port == ptp_l3_udp::EVENT_PORT);
        }
    }

    void test_l3_udp_rejections()
    {
        const uint8_t mac[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
        const uint8_t ipv4[4] = {192, 168, 8, 100};
        Frame request = make_delay_request();
        uint8_t frame[ptp_l3_udp::MAX_FRAME_SIZE];
        const size_t length = ptp_l3_udp::encapsulate(frame, sizeof(frame), mac, ipv4, false, request.data() + 14, 44);
        CHECK(length == 86);
        ptp_l3_udp::Datagram datagram{};
        ptp::ParsedMessage parsed{};
        uint8_t copy[ptp_l3_udp::MAX_FRAME_SIZE];

        // Not PTP: another port, another group, another protocol, ARP -> Ignored (left to lwIP)
        std::memcpy(copy, frame, length);
        copy[14 + 20 + 3] = 123; // destination port NTP
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Ignored);
        std::memcpy(copy, frame, length);
        copy[14 + 19] = 130; // 224.0.1.130
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Ignored);
        std::memcpy(copy, frame, length);
        copy[14 + 9] = 6; // TCP
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Ignored);
        std::memcpy(copy, frame, length);
        copy[12] = 0x08;
        copy[13] = 0x06;
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Ignored);

        // PTP but broken: bad UDP checksum, bad IPv4 header checksum, fragment, multicast source MAC, wrong
        // destination MAC, a Delay_Req on the general port -> Invalid
        std::memcpy(copy, frame, length);
        copy[length - 1] ^= 1;
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Invalid);
        std::memcpy(copy, frame, length);
        copy[14 + 10] ^= 1;
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Invalid);
        std::memcpy(copy, frame, length);
        copy[14 + 6] = 0x20; // more fragments
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Invalid);
        std::memcpy(copy, frame, length);
        copy[6] |= 1;
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Invalid);
        std::memcpy(copy, frame, length);
        copy[5] = 0x82;
        CHECK(ptp_l3_udp::decode(copy, length, 7, nullptr, datagram, parsed) == ptp::MessageKind::Invalid);
        // Truncated frame
        CHECK(ptp_l3_udp::decode(frame, length - 1, 7, nullptr, datagram, parsed) == ptp::MessageKind::Invalid);
        // Encapsulation limits
        CHECK(ptp_l3_udp::encapsulate(copy, 85, mac, ipv4, false, request.data() + 14, 44) == 0);
        CHECK(ptp_l3_udp::encapsulate(copy, sizeof(copy), mac, ipv4, false, request.data() + 14, 33) == 0);
        CHECK(ptp_l3_udp::encapsulate(copy, sizeof(copy), nullptr, ipv4, false, request.data() + 14, 44) == 0);
    }

    void test_deterministic_l3_fuzz()
    {
        uint32_t state = 0x1b2c3d4eU;
        const uint8_t mac[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
        const uint8_t ipv4[4] = {192, 168, 8, 100};
        const uint8_t ipv6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
        Frame request = make_delay_request();
        uint8_t seed[2][ptp_l3_udp::MAX_FRAME_SIZE];
        const size_t seed_length[2] = {
            ptp_l3_udp::encapsulate(seed[0], sizeof(seed[0]), mac, ipv4, false, request.data() + 14, 44),
            ptp_l3_udp::encapsulate(seed[1], sizeof(seed[1]), mac, ipv6, true, request.data() + 14, 44)};
        uint8_t frame[ptp_l3_udp::MAX_FRAME_SIZE];
        ptp_l3_udp::Datagram datagram{};
        ptp::ParsedMessage parsed{};
        for (size_t iteration = 0; iteration < 30000; ++iteration)
        {
            const size_t which = iteration & 1U;
            std::memcpy(frame, seed[which], sizeof(frame));
            const size_t flips = 1 + next_random(state) % 3;
            for (size_t i = 0; i < flips; ++i)
                frame[next_random(state) % seed_length[which]] ^= static_cast<uint8_t>(1U << (next_random(state) % 8));
            const size_t length = next_random(state) % (seed_length[which] + 1);
            const ptp::MessageKind result = ptp_l3_udp::decode(frame, length, 7, nullptr, datagram, parsed);
            CHECK(result == ptp::MessageKind::Ignored || result == ptp::MessageKind::Invalid ||
                  result == ptp::MessageKind::DelayRequest || result == ptp::MessageKind::Announce);
            if (result == ptp::MessageKind::DelayRequest || result == ptp::MessageKind::Announce)
                CHECK(datagram.message >= frame && datagram.message + datagram.length <= frame + length);
        }
    }
}

#ifndef HOST_FUZZ
int main()
{
    test_follower_lifecycle();
    test_follower_rollover_snapshots_clear_and_capacity();
    test_follower_report_serialization();
    test_client_continuity_and_deduplication();
    test_client_report_limit_and_first_arrival_order();
    test_frozen_report_periods();
    test_delay_request_validity_and_output();
    test_frame_ethernet_and_dispatch_validation();
    test_delay_request_fields();
    test_version_and_control_compatibility();
    test_announce_validation();
    test_deterministic_parser_fuzz();
    test_message_builder();
    test_delay_resp_round_trip();
    test_l3_udp_round_trip();
    test_l3_udp_rejections();
    test_deterministic_l3_fuzz();
    if (failures != 0)
        std::fprintf(stderr, "%d test checks failed\n", failures);
    else
        std::printf("PTP host tests passed\n");
    return failures == 0 ? 0 : 1;
}
#else
// The input is an Ethernet frame as ntp_ethernet_input() gets it: the L2 parser and the UDP decoder must stay within
// it, and a Delay_Req they accept must give a Delay_Resp that carries its sequence, correction and port identity.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const ptp::PortConfig port = test_port();
    uint8_t response[64];
    ptp::ParsedMessage parsed{};
    ptp::MessageKind kind = ptp::parse_l2_frame(data, size, 7, port.identity, parsed, (size & 1U) != 0);
    ptp_l3_udp::Datagram datagram{};
    if (kind == ptp::MessageKind::Ignored)
    {
        kind = ptp_l3_udp::decode(data, size, 7, port.identity, datagram, parsed);
        if (kind == ptp::MessageKind::DelayRequest || kind == ptp::MessageKind::Announce)
            CHECK(datagram.message >= data && datagram.message + datagram.length <= data + size);
    }
    if (kind == ptp::MessageKind::DelayRequest)
    {
        CHECK(parsed.domain == 7);
        CHECK(ptp::build_delay_resp(response, sizeof(response), port, parsed.sequence_id, parsed.correction,
                                    parsed.source_port_identity, 1000000000LL) == ptp::DELAY_RESP_SIZE);
        CHECK(ptp::detail::read_u16(response + 30) == parsed.sequence_id &&
              ptp::detail::read_i64(response + 8) == parsed.correction &&
              std::memcmp(response + 44, parsed.source_port_identity, 10) == 0);
    }
    if (failures != 0)
        __builtin_trap();
    return 0;
}
#endif
