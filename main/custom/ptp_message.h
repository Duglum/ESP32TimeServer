// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Builder of the PTP messages a two-step end-to-end master sends (IEEE 1588-2019): Sync, Follow_Up, Delay_Resp and
// Announce. Pure functions without ESP-IDF dependencies (host tests in testing/host/ptp_test.cpp). The fork's
// version of upstream's ptp_build_message(): the Announce carries the real quality of the grandmaster (clockClass,
// clockAccuracy, offsetScaledLogVariance, traceability), the Delay_Resp returns the correctionField of the request.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "ptp_l2_master_selection.h"

namespace ptp
{
enum MessageType : uint8_t
{
    MESSAGE_SYNC = 0,
    MESSAGE_DELAY_REQ = 1,
    MESSAGE_FOLLOW_UP = 8,
    MESSAGE_DELAY_RESP = 9,
    MESSAGE_ANNOUNCE = 11,
};

static constexpr size_t HEADER_SIZE = 34;
static constexpr size_t SYNC_SIZE = 44;
static constexpr size_t FOLLOW_UP_SIZE = 44;
static constexpr size_t DELAY_RESP_SIZE = 54;
static constexpr size_t ANNOUNCE_SIZE = 64;

// Values of the Announce (IEEE 1588-2019 7.6.2.5, 7.6.2.6, 7.6.3.3, 7.6.2.8)
static constexpr uint8_t CLOCK_CLASS_LOCKED = 6;        // synchronised to a primary reference (GNSS)
static constexpr uint8_t CLOCK_CLASS_DEFAULT = 248;     // default, not synchronised
static constexpr uint8_t CLOCK_ACCURACY_250NS = 0x22;   // within 250 ns
static constexpr uint8_t CLOCK_ACCURACY_UNKNOWN = 0xFE;
static constexpr uint16_t VARIANCE_GNSS = 0x4E5D;       // offsetScaledLogVariance of a GNSS locked grandmaster
static constexpr uint16_t VARIANCE_UNKNOWN = 0xFFFF;
static constexpr uint8_t TIME_SOURCE_GNSS = 0x20;

// Flag bits of the second flag octet (byte 7)
static constexpr uint8_t FLAG_LEAP61 = 0x01;
static constexpr uint8_t FLAG_LEAP59 = 0x02;
static constexpr uint8_t FLAG_UTC_OFFSET_VALID = 0x04;
static constexpr uint8_t FLAG_PTP_TIMESCALE = 0x08;
static constexpr uint8_t FLAG_TIME_TRACEABLE = 0x10;
static constexpr uint8_t FLAG_FREQUENCY_TRACEABLE = 0x20;
// First flag octet (byte 6)
static constexpr uint8_t FLAG_TWO_STEP = 0x02;

// One port of the master
struct PortConfig
{
    uint8_t domain = 0;
    uint8_t identity[8]{};    // clockIdentity (EUI-64 of the MAC); also the grandmasterIdentity
    uint16_t port_number = 1; // 1 = Ethernet (L2), 2 = UDP/IPv4, 3 = UDP/IPv6 (fork numbering)
    int8_t log_sync_interval = 0;
    int8_t log_announce_interval = 1;
    int8_t log_min_delay_req_interval = 0;
    MasterDataset dataset{}; // priorities and clock quality of the Announce (identity is ignored, see above)
    uint8_t time_source = TIME_SOURCE_GNSS;
    int16_t utc_offset = 37; // currentUtcOffset, TAI - UTC in s
    bool utc_offset_valid = false;
    bool leap61 = false;
    bool leap59 = false;
    bool time_traceable = false;
    bool frequency_traceable = false;
    bool authenticated = false; // minorVersionPTP 1 (messages carry an AUTHENTICATION TLV)
};

namespace detail
{
inline void put_u16(uint8_t *data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

inline void put_u64(uint8_t *data, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        data[i] = static_cast<uint8_t>(value >> (8 * (7 - i)));
}
}

// Timestamp (48 bit seconds, 32 bit nanoseconds) of a PTP time in ns. false (zeros written) for a negative time or
// one beyond the 48 bit seconds
inline bool write_timestamp(uint8_t *destination, int64_t tai_ns)
{
    std::memset(destination, 0, 10);
    if (tai_ns < 0)
        return false;
    const uint64_t seconds = static_cast<uint64_t>(tai_ns / 1000000000LL);
    const uint32_t nanoseconds = static_cast<uint32_t>(tai_ns % 1000000000LL);
    if (seconds >> 48 != 0)
        return false;
    for (size_t i = 0; i < 6; ++i)
        destination[i] = static_cast<uint8_t>(seconds >> (8 * (5 - i)));
    for (size_t i = 0; i < 4; ++i)
        destination[6 + i] = static_cast<uint8_t>(nanoseconds >> (8 * (3 - i)));
    return true;
}

// Common header (13.3). control: controlField; log_interval: logMessageInterval
inline void build_header(uint8_t *message, uint8_t type, uint16_t size, uint16_t sequence, const PortConfig &port,
                         uint8_t control, int8_t log_interval)
{
    std::memset(message, 0, size);
    message[0] = type; // majorSdoId 0
    message[1] = port.authenticated ? 0x12 : 0x02;
    detail::put_u16(message + 2, size);
    message[4] = port.domain;
    std::memcpy(message + 20, port.identity, 8);
    detail::put_u16(message + 28, port.port_number);
    detail::put_u16(message + 30, sequence);
    message[32] = control;
    message[33] = static_cast<uint8_t>(log_interval);
}

// Each builder returns the message size, 0 if the capacity is too small or a timestamp cannot be written.

// Sync of a two-step clock: origin_tai_ns is an approximate time (IEEE 1588 allows it; some followers, e.g. Windows
// W32Time, use it as a sample, so it must not be zero), the precise one follows in the Follow_Up
inline size_t build_sync(uint8_t *message, size_t capacity, const PortConfig &port, uint16_t sequence,
                         int64_t origin_tai_ns)
{
    if (capacity < SYNC_SIZE)
        return 0;
    build_header(message, MESSAGE_SYNC, SYNC_SIZE, sequence, port, 0, port.log_sync_interval);
    message[6] = FLAG_TWO_STEP;
    return write_timestamp(message + 34, origin_tai_ns) ? SYNC_SIZE : 0;
}

inline size_t build_follow_up(uint8_t *message, size_t capacity, const PortConfig &port, uint16_t sequence,
                              int64_t precise_origin_tai_ns)
{
    if (capacity < FOLLOW_UP_SIZE)
        return 0;
    build_header(message, MESSAGE_FOLLOW_UP, FOLLOW_UP_SIZE, sequence, port, 2, port.log_sync_interval);
    return write_timestamp(message + 34, precise_origin_tai_ns) ? FOLLOW_UP_SIZE : 0;
}

// Delay_Resp: sequence and correction (correctionField) of the Delay_Req, requesting: its sourcePortIdentity
inline size_t build_delay_resp(uint8_t *message, size_t capacity, const PortConfig &port, uint16_t sequence,
                               int64_t correction, const uint8_t *requesting, int64_t receive_tai_ns)
{
    if (capacity < DELAY_RESP_SIZE || requesting == nullptr)
        return 0;
    build_header(message, MESSAGE_DELAY_RESP, DELAY_RESP_SIZE, sequence, port, 3, port.log_min_delay_req_interval);
    detail::put_u64(message + 8, static_cast<uint64_t>(correction));
    std::memcpy(message + 44, requesting, 10);
    return write_timestamp(message + 34, receive_tai_ns) ? DELAY_RESP_SIZE : 0;
}

// Announce (13.5): origin_tai_ns approximate (0 allowed)
inline size_t build_announce(uint8_t *message, size_t capacity, const PortConfig &port, uint16_t sequence,
                             int64_t origin_tai_ns)
{
    if (capacity < ANNOUNCE_SIZE)
        return 0;
    build_header(message, MESSAGE_ANNOUNCE, ANNOUNCE_SIZE, sequence, port, 5, port.log_announce_interval);
    message[7] = static_cast<uint8_t>(FLAG_PTP_TIMESCALE | (port.utc_offset_valid ? FLAG_UTC_OFFSET_VALID : 0) |
                                      (port.leap61 ? FLAG_LEAP61 : 0) | (port.leap59 && !port.leap61 ? FLAG_LEAP59 : 0) |
                                      (port.time_traceable ? FLAG_TIME_TRACEABLE : 0) |
                                      (port.frequency_traceable ? FLAG_FREQUENCY_TRACEABLE : 0));
    if (origin_tai_ns > 0 && !write_timestamp(message + 34, origin_tai_ns))
        return 0;
    detail::put_u16(message + 44, static_cast<uint16_t>(port.utc_offset));
    message[47] = port.dataset.priority1;
    message[48] = port.dataset.clock_class;
    message[49] = port.dataset.clock_accuracy;
    detail::put_u16(message + 50, port.dataset.variance);
    message[52] = port.dataset.priority2;
    std::memcpy(message + 53, port.identity, 8);
    detail::put_u16(message + 61, 0); // stepsRemoved
    message[63] = port.time_source;
    return ANNOUNCE_SIZE;
}

// Dataset of this master for the master selection (ptp::MasterSelection): the identity is the clock identity
inline MasterDataset local_dataset(const PortConfig &port)
{
    MasterDataset dataset = port.dataset;
    std::memcpy(dataset.identity, port.identity, sizeof(dataset.identity));
    return dataset;
}

// clockIdentity (EUI-64) of an Ethernet MAC address: xx:xx:xx:FF:FE:xx:xx:xx (IEEE 1588-2019 7.5.2.2.2)
inline void clock_identity_from_mac(const uint8_t *mac, uint8_t *identity)
{
    std::memcpy(identity, mac, 3);
    identity[3] = 0xff;
    identity[4] = 0xfe;
    std::memcpy(identity + 5, mac + 3, 3);
}
}
