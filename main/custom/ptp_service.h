// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// PTP master of the fork (IEEE 1588-2019 default profile: end-to-end delay, two-step, multicast). Included by
// main.cpp after the hardware NTP code: it uses main.cpp's statics (Ethernet handle and transmit mutex, PTP clock
// mapping, NTP reply status). Idea and protocol code from upstream release 4.0 (CHANGELOG 4.101/4.102); the service
// differs from upstream's:
// - no time mutex: the hardware time stamps are mapped with the seqlock ptp_map_offset_at(), and s_ptp_epoch (it
//   changes with every set, step or restart of the PTP clock) tells whether a stamp still belongs to the mapping;
//   the PPS task is never kept waiting by PTP;
// - it serves only while NTP is at stratum 1 without holdover (GNSS and PPS valid) and announces the quality of a
//   GNSS-locked grandmaster (clockClass 6, timeTraceable, frequencyTraceable);
// - TAI only at the output: TAI = UTC + currentUtcOffset (GPS - UTC + 19 from the u-blox, else the setting);
// - Delay_Req rate limit per follower and an admission limit for all PTP frames; ingress and egress latencies.
// One task serves every transport (port 1 = Ethernet; ports 2 and 3 = UDP/IPv4 and UDP/IPv6 follow later).

#pragma once

#include "ptp_followers.h"
#include "ptp_l2_protocol.h"
#include "ptp_message.h"

#if !PTP_FREE_RUNNING
#error "The PTP master needs the mapping of the free-running PTP clock (PTP_FREE_RUNNING 1)"
#endif

// Test images only (never commit 1): PTP over Ethernet on without the web settings
#ifndef PTP_TEST_FORCE_L2
#define PTP_TEST_FORCE_L2 0
#endif

enum ptp_transport_t : uint8_t
{
    PTP_TRANSPORT_L2 = 0,   // Ethernet, EtherType 0x88F7, 01:1B:19:00:00:00
    PTP_TRANSPORT_UDP4 = 1, // UDP/IPv4 224.0.1.129 (later step)
    PTP_TRANSPORT_UDP6 = 2, // UDP/IPv6 ff02::181 (later step)
    PTP_TRANSPORT_COUNT = 3,
};

static const char *const PTP_TRANSPORT_NAMES[PTP_TRANSPORT_COUNT] = {"Ethernet", "UDP/IPv4", "UDP/IPv6"};

// Settings of the PTP master (all live; the web settings set them with ptp_set_config())
struct ptp_config_t
{
    bool enabled[PTP_TRANSPORT_COUNT] = {PTP_TEST_FORCE_L2 != 0, false, false};
    uint8_t domain = 0;
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;
    int8_t log_sync_interval = 0;          // 2^n s: -3 (125 ms) to 4 (16 s)
    int8_t log_announce_interval = 1;      // 0 to 4
    int8_t log_min_delay_req_interval = 0; // told to the followers in the Delay_Resp: -3 to 6
    bool master_selection = true;          // stand aside while a better master announces itself; false: always master
    uint16_t follower_timeout_s = 30;      // a follower counts as gone after this long without a Delay_Req
    uint16_t delay_req_rate_limit = 16;    // Delay_Req per second and follower (burst of the same size), 0 = no limit
    uint8_t clock_accuracy = ptp::CLOCK_ACCURACY_250NS;
    int16_t utc_offset_s = 37;             // TAI - UTC while the GNSS receiver does not tell it (NMEA)
    int32_t ingress_latency_ns = 0;        // subtracted from the receive time stamp (T4)
    int32_t egress_latency_ns = 0;         // added to the transmit time stamp (T1)
};

// Why a port does not serve (the first reason that applies)
enum class ptp_reason_t : uint8_t
{
    serving = 0,
    disabled,
    no_link,           // Ethernet down or in recovery
    clock_not_ready,   // PTP clock not running, or its mapping to true time not valid (after a set or step)
    not_synchronized,  // the NTP reply status is not stratum 1 (GNSS, PPS)
    holdover,          // PPS holdover: PTP does not coast, the followers switch to another master
    listening,         // master selection: waiting for Announces of other masters after a (re)start
    standby,           // master selection: a better master announces itself
};

static const char *ptp_reason_name(ptp_reason_t reason)
{
    switch (reason)
    {
    case ptp_reason_t::serving:
        return "serving";
    case ptp_reason_t::disabled:
        return "disabled";
    case ptp_reason_t::no_link:
        return "no Ethernet link";
    case ptp_reason_t::clock_not_ready:
        return "PTP clock not mapped to true time";
    case ptp_reason_t::not_synchronized:
        return "not synchronized (stratum 1 needs GNSS and PPS)";
    case ptp_reason_t::holdover:
        return "PPS holdover";
    case ptp_reason_t::listening:
        return "listening for other masters";
    case ptp_reason_t::standby:
        return "a better master is present";
    }
    return "?";
}

struct ptp_counters_t
{
    std::atomic<uint32_t> sync_sent{0};
    std::atomic<uint32_t> follow_up_sent{0};
    std::atomic<uint32_t> announce_sent{0};
    std::atomic<uint32_t> delay_req_received{0};
    std::atomic<uint32_t> delay_resp_sent{0};
    std::atomic<uint32_t> rate_limited{0};        // Delay_Req over the rate limit of its follower (not answered)
    std::atomic<uint32_t> queue_drops{0};         // receive queue full or over the admission limit
    std::atomic<uint32_t> invalid{0};             // malformed Delay_Req / Announce
    std::atomic<uint32_t> ignored{0};             // other message types, other domains
    std::atomic<uint32_t> rx_timestamp_misses{0}; // Delay_Req without a usable receive time stamp (or the clock changed)
    std::atomic<uint32_t> tx_timestamp_misses{0}; // Sync without a usable transmit time stamp (no Follow_Up)
    std::atomic<uint32_t> transmit_failures{0};
    std::atomic<uint32_t> foreign_announces{0};   // Announces of other masters in our domain
};

static constexpr size_t PTP_FOLLOWERS_PER_TRANSPORT = 32;
static constexpr size_t PTP_FOLLOWER_CAPACITY = PTP_FOLLOWERS_PER_TRANSPORT * PTP_TRANSPORT_COUNT;
static constexpr size_t PTP_FOREIGN_MASTER_CAPACITY = 8;
static constexpr size_t PTP_RX_QUEUE_DEPTH = 16;
// Largest frame handed to the task: Ethernet + IPv6 + UDP + Announce + AUTHENTICATION TLV
static constexpr size_t PTP_RX_FRAME_MAX = 14 + 40 + 8 + ptp::ANNOUNCE_SIZE + ptp::AUTH_TLV_SIZE;
static constexpr size_t PTP_TX_FRAME_MAX = PTP_RX_FRAME_MAX;
static constexpr uint32_t PTP_ADMISSION_PER_100MS = 100; // all PTP frames of all transports: 1000 per second
static constexpr size_t PTP_RATE_TABLE_SIZE = 32;
static constexpr uint32_t PTP_TASK_PRIORITY = 12; // below the hardware NTP task (17), the GNSS sync (15) and the PPS task (14)
static constexpr uint8_t PTP_L2_MULTICAST_MAC[6] = {0x01, 0x1b, 0x19, 0x00, 0x00, 0x00};

struct ptp_rx_item_t
{
    uint8_t data[PTP_RX_FRAME_MAX];
    uint8_t length;
    uint8_t transport;
    eth_mac_time_t timestamp; // zero: no hardware receive time stamp
    uint32_t epoch;           // s_ptp_epoch when the frame was received
};

static ptp_counters_t s_ptp_counters[PTP_TRANSPORT_COUNT];
static std::atomic<bool> s_ptp_transport_enabled[PTP_TRANSPORT_COUNT] = {};
static std::atomic<uint8_t> s_ptp_reason[PTP_TRANSPORT_COUNT] = {};
static std::atomic<int16_t> s_ptp_utc_offset_in_use{37};
static std::atomic<bool> s_ptp_utc_offset_from_gnss{false};
static QueueHandle_t s_ptp_rx_queue = nullptr;
static uint8_t s_ptp_source_mac[6] = {};
static uint8_t s_ptp_identity[8] = {};
static std::atomic<bool> s_ptp_started{false};

// Settings, follower table and rate table: guarded by this mutex (never taken in an interrupt or for long; the PTP
// task does not hold it while it transmits)
static SemaphoreHandle_t ptp_mutex()
{
    static StaticSemaphore_t buffer;
    static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&buffer);
    return handle;
}

static ptp_config_t s_ptp_config;                  // guarded by ptp_mutex()
static std::atomic<uint32_t> s_ptp_config_generation{0};
static ptp::FollowerTable<PTP_FOLLOWER_CAPACITY, PTP_FOLLOWER_CAPACITY> s_ptp_followers(30LL * 1000000LL); // ptp_mutex()

// Takes over new settings (any task)
static void ptp_set_config(const ptp_config_t &config)
{
    if (xSemaphoreTake(ptp_mutex(), portMAX_DELAY) != pdTRUE)
        return;
    s_ptp_config = config;
    for (size_t t = 0; t < PTP_TRANSPORT_COUNT; ++t)
        s_ptp_transport_enabled[t].store(config.enabled[t], std::memory_order_release);
    s_ptp_config_generation.fetch_add(1, std::memory_order_acq_rel);
    xSemaphoreGive(ptp_mutex());
}

static void ptp_get_config(ptp_config_t &config, uint32_t &generation)
{
    if (xSemaphoreTake(ptp_mutex(), portMAX_DELAY) != pdTRUE)
        return;
    config = s_ptp_config;
    generation = s_ptp_config_generation.load(std::memory_order_acquire);
    xSemaphoreGive(ptp_mutex());
}

// ---- Receive (Ethernet input task) ----

// Called for every received frame by ntp_ethernet_input(). true: the frame is PTP of an enabled transport and was
// taken (queued or dropped); the caller frees it. false: not for PTP, it goes on to NTP / lwIP as before.
static bool ptp_receive_frame(const uint8_t *frame, uint32_t length, const eth_mac_time_t *timestamp)
{
    if (length < 14 + ptp::HEADER_SIZE || frame[12] != 0x88 || frame[13] != 0xf7)
        return false;
    const uint8_t transport = PTP_TRANSPORT_L2;
    if (!s_ptp_transport_enabled[transport].load(std::memory_order_acquire) || s_ptp_rx_queue == nullptr)
        return false;
    ptp_counters_t &counters = s_ptp_counters[transport];
    const uint8_t *message = frame + 14;

    // Only Delay_Req and Announce are of interest to a master: Sync, Follow_Up, Delay_Resp (of other masters),
    // Pdelay, Signaling and Management are dropped here
    const uint8_t type = message[0] & 0x0f;
    if (type != ptp::MESSAGE_DELAY_REQ && type != ptp::MESSAGE_ANNOUNCE)
    {
        counters.ignored.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Admission limit of all PTP frames (this runs in the one Ethernet input task: plain statics)
    static int64_t window_start_us = 0;
    static uint32_t window_count = 0;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - window_start_us >= 100000)
    {
        window_start_us = now_us;
        window_count = 0;
    }
    if (++window_count > PTP_ADMISSION_PER_100MS || length > PTP_RX_FRAME_MAX)
    {
        (length > PTP_RX_FRAME_MAX ? counters.invalid : counters.queue_drops).fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Static: only the Ethernet input task gets here, and xQueueSend() copies the item (keeps its stack small)
    static ptp_rx_item_t item;
    memcpy(item.data, frame, length);
    item.length = static_cast<uint8_t>(length);
    item.transport = transport;
    item.timestamp = timestamp != nullptr ? *timestamp : eth_mac_time_t{};
    item.epoch = s_ptp_epoch.load(std::memory_order_acquire);
    if (xQueueSend(s_ptp_rx_queue, &item, 0) != pdTRUE)
        counters.queue_drops.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Time ----

static int64_t ptp_raw_ns(const eth_mac_time_t &timestamp)
{
    return static_cast<int64_t>(timestamp.seconds) * 1000000000LL + static_cast<int64_t>(timestamp.nanoseconds);
}

// TAI time of a raw PTP clock reading: false while the mapping is not valid (never the raw reading)
static bool ptp_tai_ns_from_raw(int64_t raw_ns, int16_t utc_offset_s, int64_t &tai_ns)
{
    if (!s_ptp_comp_valid.load(std::memory_order_acquire))
        return false;
    int64_t offset_ns = 0;
    if (!ptp_map_offset_at(raw_ns, offset_ns))
        return false;
    const int64_t unix_ns = raw_ns - offset_ns;
    if (unix_ns <= 0)
        return false;
    tai_ns = unix_ns + static_cast<int64_t>(utc_offset_s) * 1000000000LL;
    return true;
}

// TAI - UTC: from the u-blox receiver (GPS - UTC of NAV-PVT + 19) when it knows it, else the setting
static int16_t ptp_utc_offset(const ptp_config_t &config)
{
    if (s_gnss_ublox)
    {
        const int16_t gps_utc = gnss_ubx_leap_seconds();
        if (gps_utc >= 0 && gps_utc <= 80)
        {
            s_ptp_utc_offset_from_gnss.store(true, std::memory_order_relaxed);
            s_ptp_utc_offset_in_use.store(static_cast<int16_t>(gps_utc + 19), std::memory_order_relaxed);
            return static_cast<int16_t>(gps_utc + 19);
        }
    }
    s_ptp_utc_offset_from_gnss.store(false, std::memory_order_relaxed);
    s_ptp_utc_offset_in_use.store(config.utc_offset_s, std::memory_order_relaxed);
    return config.utc_offset_s;
}

// Whether the time may be served at all (the same for every transport)
static ptp_reason_t ptp_time_reason()
{
    if (!s_ethernet_connected.load(std::memory_order_acquire) ||
        s_ethernet_recovery_in_progress.load(std::memory_order_acquire))
        return ptp_reason_t::no_link;
    if (!s_ptp_clock_ready.load(std::memory_order_acquire) || !s_ptp_comp_valid.load(std::memory_order_acquire))
        return ptp_reason_t::clock_not_ready;
    const ntp_reply_status_t status = get_ntp_reply_status();
    if (status.stratum != 1 || !status.serve)
        return ptp_reason_t::not_synchronized;
    if (status.holdover)
        return ptp_reason_t::holdover;
    return ptp_reason_t::serving;
}

// ---- Transmit ----

// Sends a PTP message on a transport. timestamp != nullptr: with the hardware transmit time stamp (left zero if the
// EMAC gave none or it is implausible). The transmit mutex of the hardware NTP path is taken for a few ms at most:
// a Sync that cannot be sent in time is skipped (counted), nothing waits longer.
static bool ptp_send(uint8_t transport, const uint8_t *message, size_t message_length, eth_mac_time_t *timestamp)
{
    if (timestamp != nullptr)
        *timestamp = eth_mac_time_t{};
    uint8_t frame[PTP_TX_FRAME_MAX];
    size_t frame_length = 0;
    if (transport == PTP_TRANSPORT_L2)
    {
        if (14 + message_length > sizeof(frame))
            return false;
        memcpy(frame, PTP_L2_MULTICAST_MAC, 6);
        memcpy(frame + 6, s_ptp_source_mac, 6);
        frame[12] = 0x88;
        frame[13] = 0xf7;
        memcpy(frame + 14, message, message_length);
        frame_length = 14 + message_length;
        if (frame_length < 60)
        {
            memset(frame + frame_length, 0, 60 - frame_length);
            frame_length = 60;
        }
    }
    else
        return false;

    const esp_eth_handle_t handle = s_eth_handle;
    if (handle == nullptr || s_hardware_ntp_transmit_mutex == nullptr ||
        !s_ethernet_connected.load(std::memory_order_acquire) ||
        s_ethernet_recovery_in_progress.load(std::memory_order_acquire))
        return false;
    if (xSemaphoreTake(s_hardware_ntp_transmit_mutex, pdMS_TO_TICKS(5)) != pdTRUE)
    {
        s_ptp_counters[transport].transmit_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    esp_err_t result = ESP_ERR_INVALID_STATE;
    // Reserve behind the time stamp, as in transmit_with_hw_timestamp(): protects the stack if ctrl is larger
    struct
    {
        eth_mac_time_t ts;
        uint8_t guard[56];
    } ctrl{};
    if (s_ptp_clock_ready.load(std::memory_order_acquire) &&
        !s_ethernet_recovery_in_progress.load(std::memory_order_acquire))
    {
        result = timestamp != nullptr
                     ? esp_eth_transmit_ctrl_vargs(handle, &ctrl, 2, frame, static_cast<uint32_t>(frame_length))
                     : esp_eth_transmit(handle, frame, static_cast<uint32_t>(frame_length));
    }
    xSemaphoreGive(s_hardware_ntp_transmit_mutex);
    if (result != ESP_OK)
    {
        s_ptp_counters[transport].transmit_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (timestamp != nullptr && timestamp_is_valid(&ctrl.ts))
    {
        // Plausible: at most 100 ms old (like the NTP path)
        struct timespec now_ptp{};
        if (clock_gettime(CLOCK_PTP_SYSTEM, &now_ptp) == 0)
        {
            const int64_t age_ns = static_cast<int64_t>(now_ptp.tv_sec) * 1000000000LL + now_ptp.tv_nsec - ptp_raw_ns(ctrl.ts);
            if (age_ns > -2000000LL && age_ns < 100000000LL)
                *timestamp = ctrl.ts;
        }
    }
    return true;
}

// ---- Rate limit of Delay_Req per follower (PTP task only) ----

struct ptp_rate_entry_t
{
    uint8_t identity[10];
    uint8_t transport;
    bool used;
    int64_t last_us;
    int64_t tokens_us; // credit in us of the refill period (one request = 1 s / limit)
};
static ptp_rate_entry_t s_ptp_rate_table[PTP_RATE_TABLE_SIZE];

// Token bucket of `limit` requests per second with a burst of `limit`; a follower not in the table (or one that
// replaces the oldest entry) starts with a full bucket. Spoofed identities only refill buckets: the admission limit
// of ptp_receive_frame() bounds the total.
static bool ptp_rate_admit(const uint8_t *identity, uint8_t transport, int64_t now_us, uint16_t limit)
{
    if (limit == 0)
        return true;
    const int64_t cost_us = 1000000LL / limit;
    const int64_t burst_us = cost_us * limit;
    ptp_rate_entry_t *entry = nullptr;
    ptp_rate_entry_t *oldest = &s_ptp_rate_table[0];
    for (auto &candidate : s_ptp_rate_table)
    {
        if (candidate.used && candidate.transport == transport && memcmp(candidate.identity, identity, 10) == 0)
        {
            entry = &candidate;
            break;
        }
        if (!candidate.used || (oldest->used && candidate.last_us < oldest->last_us))
            oldest = &candidate;
    }
    if (entry == nullptr)
    {
        entry = oldest;
        memcpy(entry->identity, identity, 10);
        entry->transport = transport;
        entry->used = true;
        entry->tokens_us = burst_us;
    }
    else
        entry->tokens_us = std::min(burst_us, entry->tokens_us + std::max<int64_t>(0, now_us - entry->last_us));
    entry->last_us = now_us;
    if (entry->tokens_us < cost_us)
        return false;
    entry->tokens_us -= cost_us;
    return true;
}

// ---- Service task ----

struct ptp_port_t
{
    ptp::MasterSelection<PTP_FOREIGN_MASTER_CAPACITY> selection{true, 10000000LL, 4000000LL};
    uint16_t sync_sequence = 0;
    uint16_t announce_sequence = 0;
    int64_t next_sync_us = 0;
    int64_t next_announce_us = 0;
    uint32_t config_generation = UINT32_MAX; // settings and clock epoch of the last selection reset
    uint32_t epoch = UINT32_MAX;
    bool enabled = false;
    ptp_reason_t reason = ptp_reason_t::disabled;
};

static ptp::PortConfig ptp_port_config(const ptp_config_t &config, uint8_t transport, int16_t utc_offset)
{
    ptp::PortConfig port{};
    port.domain = config.domain;
    memcpy(port.identity, s_ptp_identity, sizeof(port.identity));
    port.port_number = static_cast<uint16_t>(transport + 1);
    port.log_sync_interval = config.log_sync_interval;
    port.log_announce_interval = config.log_announce_interval;
    port.log_min_delay_req_interval = config.log_min_delay_req_interval;
    port.dataset.priority1 = config.priority1;
    port.dataset.priority2 = config.priority2;
    // Only sent while serving, i.e. synchronized to GNSS with PPS
    port.dataset.clock_class = ptp::CLOCK_CLASS_LOCKED;
    port.dataset.clock_accuracy = config.clock_accuracy;
    port.dataset.variance = ptp::VARIANCE_GNSS;
    port.time_source = ptp::TIME_SOURCE_GNSS;
    port.utc_offset = utc_offset;
    port.utc_offset_valid = true;
    port.time_traceable = true;
    port.frequency_traceable = true;
    return port;
}

static int64_t ptp_interval_us(int8_t log_interval)
{
    return log_interval >= 0 ? 1000000LL << log_interval : 1000000LL >> -log_interval;
}

static void ptp_clear_followers(uint8_t transport)
{
    if (xSemaphoreTake(ptp_mutex(), portMAX_DELAY) != pdTRUE)
        return;
    s_ptp_followers.clear_transport(transport);
    xSemaphoreGive(ptp_mutex());
}

// Sync and Follow_Up: the Follow_Up carries the hardware transmit time stamp of the Sync (+ egress latency)
static void ptp_send_sync(ptp_port_t &port, const ptp::PortConfig &port_config, const ptp_config_t &config, uint8_t transport)
{
    ptp_counters_t &counters = s_ptp_counters[transport];
    const uint16_t sequence = port.sync_sequence++;
    const uint32_t epoch = s_ptp_epoch.load(std::memory_order_acquire);
    struct timespec now_raw{};
    int64_t origin_tai_ns = 0;
    if (clock_gettime(CLOCK_PTP_SYSTEM, &now_raw) != 0 ||
        !ptp_tai_ns_from_raw(static_cast<int64_t>(now_raw.tv_sec) * 1000000000LL + now_raw.tv_nsec, port_config.utc_offset, origin_tai_ns))
        return;
    uint8_t message[ptp::ANNOUNCE_SIZE];
    const size_t length = ptp::build_sync(message, sizeof(message), port_config, sequence, origin_tai_ns);
    eth_mac_time_t egress{};
    if (length == 0 || !ptp_send(transport, message, length, &egress))
        return;
    counters.sync_sent.fetch_add(1, std::memory_order_relaxed);
    int64_t precise_tai_ns = 0;
    if (!timestamp_is_valid(&egress) || epoch != s_ptp_epoch.load(std::memory_order_acquire) ||
        !ptp_tai_ns_from_raw(ptp_raw_ns(egress), port_config.utc_offset, precise_tai_ns))
    {
        counters.tx_timestamp_misses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const size_t follow_up = ptp::build_follow_up(message, sizeof(message), port_config, sequence,
                                                  precise_tai_ns + config.egress_latency_ns);
    if (follow_up != 0 && ptp_send(transport, message, follow_up, nullptr))
        counters.follow_up_sent.fetch_add(1, std::memory_order_relaxed);
}

static void ptp_send_announce(ptp_port_t &port, const ptp::PortConfig &port_config, uint8_t transport)
{
    uint8_t message[ptp::ANNOUNCE_SIZE];
    struct timespec now_raw{};
    int64_t origin_tai_ns = 0;
    if (clock_gettime(CLOCK_PTP_SYSTEM, &now_raw) == 0)
        (void)ptp_tai_ns_from_raw(static_cast<int64_t>(now_raw.tv_sec) * 1000000000LL + now_raw.tv_nsec, port_config.utc_offset, origin_tai_ns);
    const size_t length = ptp::build_announce(message, sizeof(message), port_config, port.announce_sequence++, origin_tai_ns);
    if (length != 0 && ptp_send(transport, message, length, nullptr))
        s_ptp_counters[transport].announce_sent.fetch_add(1, std::memory_order_relaxed);
}

// A received Delay_Req or Announce
static void ptp_handle_item(const ptp_rx_item_t &item, ptp_port_t &port, const ptp::PortConfig &port_config,
                            const ptp_config_t &config, bool serving, int64_t now_us)
{
    ptp_counters_t &counters = s_ptp_counters[item.transport];
    ptp::ParsedMessage parsed{};
    const uint8_t *source_mac = item.data + 6;
    const ptp::MessageKind kind = ptp::parse_l2_frame(item.data, item.length, config.domain, s_ptp_identity, parsed);
    if (kind == ptp::MessageKind::Invalid)
    {
        counters.invalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (kind == ptp::MessageKind::Ignored)
    {
        counters.ignored.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (kind == ptp::MessageKind::Announce)
    {
        counters.foreign_announces.fetch_add(1, std::memory_order_relaxed);
        if (config.master_selection && item.epoch == s_ptp_epoch.load(std::memory_order_acquire))
            port.selection.observe(parsed.announce_dataset, parsed.source_port_identity, parsed.sequence_id,
                                   parsed.log_announce_interval, now_us);
        return;
    }

    counters.delay_req_received.fetch_add(1, std::memory_order_relaxed);
    if (!serving)
        return;
    int64_t receive_tai_ns = 0;
    if (!timestamp_is_valid(&item.timestamp) || item.epoch != s_ptp_epoch.load(std::memory_order_acquire) ||
        !ptp_tai_ns_from_raw(ptp_raw_ns(item.timestamp), port_config.utc_offset, receive_tai_ns))
    {
        counters.rx_timestamp_misses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!ptp_rate_admit(parsed.source_port_identity, item.transport, now_us, config.delay_req_rate_limit))
    {
        counters.rate_limited.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint8_t message[ptp::DELAY_RESP_SIZE];
    const size_t length = ptp::build_delay_resp(message, sizeof(message), port_config, parsed.sequence_id, parsed.correction,
                                                parsed.source_port_identity, receive_tai_ns - config.ingress_latency_ns);
    if (length == 0 || !ptp_send(item.transport, message, length, nullptr))
        return;
    counters.delay_resp_sent.fetch_add(1, std::memory_order_relaxed);
    if (xSemaphoreTake(ptp_mutex(), portMAX_DELAY) == pdTRUE)
    {
        s_ptp_followers.observe(parsed.domain, parsed.source_port_identity, now_us, source_mac, item.transport,
                                static_cast<int64_t>(config.follower_timeout_s) * 1000000LL, PTP_FOLLOWERS_PER_TRANSPORT);
        xSemaphoreGive(ptp_mutex());
    }
}

// State of the ports and the received item: only the PTP task uses them (static, so the task stack stays small)
static ptp_port_t s_ptp_ports[PTP_TRANSPORT_COUNT];
static ptp_rx_item_t s_ptp_task_item;

static void ptp_service_task(void *parameter)
{
    (void)parameter;
    ptp_port_t *const ports = s_ptp_ports;
    ptp_rx_item_t &item = s_ptp_task_item;
    int64_t last_expire_us = 0;
#if DEBUG_ENABLED
    int64_t last_report_us = 0;
#endif
    for (;;)
    {
        const bool received = xQueueReceive(s_ptp_rx_queue, &item, pdMS_TO_TICKS(5)) == pdTRUE;
        ptp_config_t config;
        uint32_t generation = 0;
        ptp_get_config(config, generation);
        const int64_t now_us = esp_timer_get_time();
        const uint32_t epoch = s_ptp_epoch.load(std::memory_order_acquire);
        const ptp_reason_t time_reason = ptp_time_reason();
        const int16_t utc_offset = ptp_utc_offset(config);

        for (uint8_t transport = 0; transport < PTP_TRANSPORT_COUNT; ++transport)
        {
            ptp_port_t &port = ports[transport];
            const bool enabled = config.enabled[transport];
            const ptp::PortConfig port_config = ptp_port_config(config, transport, utc_offset);

            // New settings, a changed clock or a port switched on: the master selection starts again (it listens
            // for two of our announce intervals before it serves)
            if (enabled && (!port.enabled || port.config_generation != generation || port.epoch != epoch))
            {
                const int64_t announce_us = ptp_interval_us(config.log_announce_interval);
                port.selection = ptp::MasterSelection<PTP_FOREIGN_MASTER_CAPACITY>(true, 10000000LL, 2 * announce_us);
                port.selection.reset(ptp::local_dataset(port_config), now_us);
                port.config_generation = generation;
                port.epoch = epoch;
                port.next_sync_us = now_us;
                port.next_announce_us = now_us;
            }
            port.enabled = enabled;

            ptp_reason_t reason = !enabled ? ptp_reason_t::disabled : time_reason;
            if (reason == ptp_reason_t::serving && config.master_selection)
            {
                const ptp::SelectionRole role = port.selection.role(now_us);
                reason = role == ptp::SelectionRole::Listening ? ptp_reason_t::listening
                         : role == ptp::SelectionRole::Standby ? ptp_reason_t::standby
                                                                : ptp_reason_t::serving;
            }
            const bool serving = reason == ptp_reason_t::serving;

            if (received && item.transport == transport && enabled)
                ptp_handle_item(item, port, port_config, config, serving, now_us);

            if (serving)
            {
                const int64_t sync_us = ptp_interval_us(config.log_sync_interval);
                if (now_us >= port.next_sync_us)
                {
                    ptp_send_sync(port, port_config, config, transport);
                    port.next_sync_us = now_us - port.next_sync_us >= sync_us ? now_us + sync_us : port.next_sync_us + sync_us;
                }
                const int64_t announce_us = ptp_interval_us(config.log_announce_interval);
                if (now_us >= port.next_announce_us)
                {
                    ptp_send_announce(port, port_config, transport);
                    port.next_announce_us = now_us - port.next_announce_us >= announce_us ? now_us + announce_us
                                                                                         : port.next_announce_us + announce_us;
                }
            }

            if (reason != port.reason)
            {
                if (serving)
                    ESP_LOGI(TAG, "PTP %s: master in domain %u (clockClass 6, TAI - UTC %d s%s)", PTP_TRANSPORT_NAMES[transport],
                             static_cast<unsigned>(config.domain), static_cast<int>(utc_offset),
                             s_ptp_utc_offset_from_gnss.load(std::memory_order_relaxed) ? " from the GNSS receiver" : "");
                else if (port.reason == ptp_reason_t::serving)
                    ESP_LOGW(TAG, "PTP %s: stopped serving: %s", PTP_TRANSPORT_NAMES[transport], ptp_reason_name(reason));
                if (!serving)
                    ptp_clear_followers(transport);
                port.reason = reason;
                s_ptp_reason[transport].store(static_cast<uint8_t>(reason), std::memory_order_relaxed);
            }
        }

        if (now_us - last_expire_us >= 1000000LL && xSemaphoreTake(ptp_mutex(), portMAX_DELAY) == pdTRUE)
        {
            last_expire_us = now_us;
            s_ptp_followers.expire(now_us);
            xSemaphoreGive(ptp_mutex());
        }
#if DEBUG_ENABLED
        if (now_us - last_report_us >= 60000000LL)
        {
            last_report_us = now_us;
            for (uint8_t transport = 0; transport < PTP_TRANSPORT_COUNT; ++transport)
            {
                if (!config.enabled[transport])
                    continue;
                const ptp_counters_t &c = s_ptp_counters[transport];
                ESP_LOGI(TAG, "PTP %s: %s; sync %lu follow_up %lu announce %lu delay_req %lu delay_resp %lu rate_limited %lu "
                              "drops %lu invalid %lu ignored %lu rx_ts_miss %lu tx_ts_miss %lu tx_fail %lu foreign_announce %lu",
                         PTP_TRANSPORT_NAMES[transport], ptp_reason_name(ports[transport].reason),
                         static_cast<unsigned long>(c.sync_sent.load()), static_cast<unsigned long>(c.follow_up_sent.load()),
                         static_cast<unsigned long>(c.announce_sent.load()), static_cast<unsigned long>(c.delay_req_received.load()),
                         static_cast<unsigned long>(c.delay_resp_sent.load()), static_cast<unsigned long>(c.rate_limited.load()),
                         static_cast<unsigned long>(c.queue_drops.load()), static_cast<unsigned long>(c.invalid.load()),
                         static_cast<unsigned long>(c.ignored.load()), static_cast<unsigned long>(c.rx_timestamp_misses.load()),
                         static_cast<unsigned long>(c.tx_timestamp_misses.load()), static_cast<unsigned long>(c.transmit_failures.load()),
                         static_cast<unsigned long>(c.foreign_announces.load()));
            }
        }
#endif
        task_stack_checkpoint(PTP_Service);
    }
}

// Started once after the hardware time stamps were set up (the Ethernet MAC address is known)
static void start_ptp_service()
{
    if (s_ptp_started.load(std::memory_order_acquire) || s_eth_handle == nullptr)
        return;
    if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, s_ptp_source_mac) != ESP_OK)
    {
        ESP_LOGE(TAG, "PTP: Ethernet MAC address not available, PTP disabled");
        return;
    }
    ptp::clock_identity_from_mac(s_ptp_source_mac, s_ptp_identity);
    s_ptp_rx_queue = xQueueCreate(PTP_RX_QUEUE_DEPTH, sizeof(ptp_rx_item_t));
    if (s_ptp_rx_queue == nullptr ||
        xTaskCreatePinnedToCore(ptp_service_task, "ptp_service", PTP_Service_Task_Stack_Size, nullptr, PTP_TASK_PRIORITY,
                                nullptr, tskNO_AFFINITY) != pdPASS)
    {
        if (s_ptp_rx_queue != nullptr)
            vQueueDelete(s_ptp_rx_queue);
        s_ptp_rx_queue = nullptr;
        ESP_LOGE(TAG, "PTP: service could not be started");
        return;
    }
    s_ptp_started.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "PTP: clock identity %02x%02x%02x.%02x%02x.%02x%02x%02x, %s", s_ptp_identity[0], s_ptp_identity[1],
             s_ptp_identity[2], s_ptp_identity[3], s_ptp_identity[4], s_ptp_identity[5], s_ptp_identity[6], s_ptp_identity[7],
             s_ptp_transport_enabled[PTP_TRANSPORT_L2].load() ? "Ethernet on" : "off");
}

// ---- Web admin ----

// Settings of the web page into the PTP master (app_apply_live_settings, at the start and with every Apply)
static void ptp_apply_settings(const web_settings_t &settings)
{
    ptp_config_t config;
    config.enabled[PTP_TRANSPORT_L2] = settings.ptp_l2_enabled || PTP_TEST_FORCE_L2 != 0;
    // UDP: not served yet (next step of the PTP backport); the settings exist already
    config.enabled[PTP_TRANSPORT_UDP4] = false;
    config.enabled[PTP_TRANSPORT_UDP6] = false;
    config.domain = settings.ptp_domain;
    config.priority1 = settings.ptp_priority1;
    config.priority2 = settings.ptp_priority2;
    config.log_sync_interval = settings.ptp_log_sync_interval;
    config.log_announce_interval = settings.ptp_log_announce_interval;
    config.log_min_delay_req_interval = settings.ptp_log_min_delay_req_interval;
    config.master_selection = settings.ptp_master_selection;
    config.follower_timeout_s = settings.ptp_follower_timeout_s;
    config.delay_req_rate_limit = settings.ptp_delay_req_rate_limit;
    config.clock_accuracy = settings.ptp_clock_accuracy;
    config.utc_offset_s = settings.ptp_utc_offset_s;
    config.ingress_latency_ns = settings.ptp_ingress_latency_ns;
    config.egress_latency_ns = settings.ptp_egress_latency_ns;
    ptp_set_config(config);
}

void app_get_ptp_status(web_ptp_status_t &status)
{
    status.started = s_ptp_started.load(std::memory_order_acquire);
    memcpy(status.identity, s_ptp_identity, sizeof(status.identity));
    status.utc_offset = s_ptp_utc_offset_in_use.load(std::memory_order_relaxed);
    status.utc_offset_from_gnss = s_ptp_utc_offset_from_gnss.load(std::memory_order_relaxed);
    for (size_t t = 0; t < PTP_TRANSPORT_COUNT && t < WEB_PTP_TRANSPORTS; ++t)
    {
        const ptp_counters_t &c = s_ptp_counters[t];
        web_ptp_port_t &port = status.ports[t];
        port.enabled = s_ptp_transport_enabled[t].load(std::memory_order_acquire);
        port.state = ptp_reason_name(port.enabled ? static_cast<ptp_reason_t>(s_ptp_reason[t].load(std::memory_order_relaxed))
                                                  : ptp_reason_t::disabled);
        port.followers = 0;
        port.sync_sent = c.sync_sent.load(std::memory_order_relaxed);
        port.follow_up_sent = c.follow_up_sent.load(std::memory_order_relaxed);
        port.announce_sent = c.announce_sent.load(std::memory_order_relaxed);
        port.delay_req_received = c.delay_req_received.load(std::memory_order_relaxed);
        port.delay_resp_sent = c.delay_resp_sent.load(std::memory_order_relaxed);
        port.rate_limited = c.rate_limited.load(std::memory_order_relaxed);
        port.queue_drops = c.queue_drops.load(std::memory_order_relaxed);
        port.invalid = c.invalid.load(std::memory_order_relaxed);
        port.ignored = c.ignored.load(std::memory_order_relaxed);
        port.rx_timestamp_misses = c.rx_timestamp_misses.load(std::memory_order_relaxed);
        port.tx_timestamp_misses = c.tx_timestamp_misses.load(std::memory_order_relaxed);
        port.transmit_failures = c.transmit_failures.load(std::memory_order_relaxed);
        port.foreign_announces = c.foreign_announces.load(std::memory_order_relaxed);
    }
    status.follower_count = 0;
    const int64_t now_us = esp_timer_get_time();
    if (xSemaphoreTake(ptp_mutex(), pdMS_TO_TICKS(100)) != pdTRUE)
        return;
    s_ptp_followers.for_each_active([&](uint8_t transport, uint8_t, const uint8_t *identity, const uint8_t *mac, int64_t last_seen_us)
                                    {
        if (transport < WEB_PTP_TRANSPORTS)
            ++status.ports[transport].followers;
        if (status.follower_count >= WEB_PTP_FOLLOWERS)
            return;
        web_ptp_follower_t &follower = status.followers[status.follower_count++];
        follower.transport = transport;
        memcpy(follower.mac, mac, sizeof(follower.mac));
        memcpy(follower.port_identity, identity, sizeof(follower.port_identity));
        follower.last_seen_s = now_us > last_seen_us ? static_cast<uint32_t>((now_us - last_seen_us) / 1000000LL) : 0; });
    xSemaphoreGive(ptp_mutex());
}
