// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- UBX protocol of u-blox receivers (M10 and newer: configuration with CFG-VALSET / CFG-VALGET only) ----
// The reader task of custom/gnss_uart hands every frame with a correct checksum over. The periodic messages NAV-PVT
// (time label and validity), NAV-TIMEUTC (validUTC) and TIM-TP (quantization error of the next time pulse) are kept;
// commands wait for their answer (poll response, ACK or NAK). Commands are serialized; they must not be sent from the
// reader task.

// Configuration layers (CFG-VALSET bit field, CFG-VALGET layer number)
static constexpr uint8_t GNSS_UBX_LAYER_RAM = 0x01;
static constexpr uint8_t GNSS_UBX_LAYER_BBR = 0x02;

// Configuration keys (u-blox M10 interface description). Bits 28..30 of a key give the size of its value.
static constexpr uint32_t GNSS_UBX_CFG_UART1_BAUDRATE = 0x40520001;     // U4
static constexpr uint32_t GNSS_UBX_CFG_UART1INPROT_UBX = 0x10730001;    // L
static constexpr uint32_t GNSS_UBX_CFG_UART1OUTPROT_UBX = 0x10740001;   // L
static constexpr uint32_t GNSS_UBX_CFG_UART1OUTPROT_NMEA = 0x10740002;  // L
static constexpr uint32_t GNSS_UBX_CFG_I2COUTPROT_UBX = 0x10720001;     // L
static constexpr uint32_t GNSS_UBX_CFG_I2COUTPROT_NMEA = 0x10720002;    // L
static constexpr uint32_t GNSS_UBX_CFG_UART2OUTPROT_UBX = 0x10760001;   // L (the M10 has no UART2: NAK)
static constexpr uint32_t GNSS_UBX_CFG_UART2OUTPROT_NMEA = 0x10760002;  // L
static constexpr uint32_t GNSS_UBX_CFG_NAVSPG_DYNMODEL = 0x20110021;    // E1
static constexpr uint32_t GNSS_UBX_CFG_MSGOUT_NAV_PVT_UART1 = 0x20910007;     // U1, rate per navigation epoch
static constexpr uint32_t GNSS_UBX_CFG_MSGOUT_NAV_TIMEUTC_UART1 = 0x2091005c; // U1
static constexpr uint32_t GNSS_UBX_CFG_MSGOUT_TIM_TP_UART1 = 0x2091017e;      // U1

// Dynamic platform models (CFG-NAVSPG-DYNMODEL)
static constexpr uint8_t GNSS_UBX_DYNMODEL_PORTABLE = 0;
static constexpr uint8_t GNSS_UBX_DYNMODEL_STATIONARY = 2;
static constexpr uint8_t GNSS_UBX_DYNMODEL_PEDESTRIAN = 3;
static constexpr uint8_t GNSS_UBX_DYNMODEL_AUTOMOTIVE = 4;
static constexpr uint8_t GNSS_UBX_DYNMODEL_SEA = 5;
static constexpr uint8_t GNSS_UBX_DYNMODEL_AIRBORNE1G = 6;
static constexpr uint8_t GNSS_UBX_DYNMODEL_AIRBORNE2G = 7;
static constexpr uint8_t GNSS_UBX_DYNMODEL_AIRBORNE4G = 8;
static constexpr uint8_t GNSS_UBX_DYNMODEL_WRIST = 9;
static constexpr uint8_t GNSS_UBX_DYNMODEL_BIKE = 10;
static constexpr uint8_t GNSS_UBX_DYNMODEL_MOWER = 11;
static constexpr uint8_t GNSS_UBX_DYNMODEL_ESCOOTER = 12;
static constexpr uint8_t GNSS_UBX_DYNMODEL_RAIL = 13;

struct gnss_ubx_config_t
{
    void (*on_satellites)(uint8_t count); // satellites used in the navigation solution (NAV-PVT); may be NULL
};

// Sets the callback and forgets the received messages (before the UART is started)
void gnss_ubx_init(const gnss_ubx_config_t &config);

// Reader task of custom/gnss_uart: a frame with a correct checksum and the flush generation at its start
void gnss_ubx_process_frame(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length, uint32_t generation);

// ---- Commands (each one waits for its answer up to timeout_ms) ----

struct gnss_ubx_cfg_item_t
{
    uint32_t key;
    uint32_t value; // the size comes from the key (L and U1/E1: 1 byte, U2: 2, U4: 4)
};

// CFG-VALSET of up to 8 items in the given layers (GNSS_UBX_LAYER_*). true = ACK.
bool gnss_ubx_val_set(const gnss_ubx_cfg_item_t *items, size_t count, uint8_t layers, uint32_t timeout_ms);
// CFG-VALGET of one key from one layer (GNSS_UBX_LAYER_RAM = 0 in the poll). false = NAK or no answer.
bool gnss_ubx_val_get(uint32_t key, uint8_t layer, uint32_t *value, uint32_t timeout_ms);

// SEC-UNIQID as upper case hexadecimal text (5 bytes of version 1, 6 bytes of the M10, as the SparkFun library did)
bool gnss_ubx_query_unique_id(char *text, size_t text_size, uint32_t timeout_ms);

struct gnss_ubx_version_t
{
    char firmware_type[8]; // e.g. "SPG" (FWVER, at most 7 characters)
    uint8_t firmware_high;
    uint8_t firmware_low;
    uint8_t protocol_high; // PROTVER
    uint8_t protocol_low;
    char module[14];       // MOD, at most 13 characters; "NONE" if the receiver does not report it
};

// MON-VER: needs FWVER and PROTVER in the extensions (as the SparkFun library did)
bool gnss_ubx_query_version(gnss_ubx_version_t *version, uint32_t timeout_ms);

// ---- Periodic messages ----

struct gnss_ubx_pvt_t
{
    uint32_t itow_ms;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    int32_t nano;
    uint8_t valid;    // bit 0 validDate, bit 1 validTime, bit 2 fullyResolved
    uint8_t fix_type; // 0 none, 1 dead reckoning, 2 2D, 3 3D, 4 GNSS + dead reckoning, 5 time only
    uint8_t flags;    // bit 0 gnssFixOK
    uint8_t flags2;   // bit 5 confirmedAvai, bit 7 confirmedTime
    uint8_t num_sv;
    int64_t rx_us;    // esp_timer time of the reception (0 = none)
    uint32_t generation;
};

// Latest NAV-PVT, at most max_age_us old
bool gnss_ubx_latest_pvt(gnss_ubx_pvt_t *pvt, int64_t max_age_us);
// Waits until deadline_us (esp_timer) for the first NAV-PVT that started after the flush that returned generation
bool gnss_ubx_wait_pvt(uint32_t generation, int64_t deadline_us, gnss_ubx_pvt_t *pvt);

// Reasons why a NAV-PVT does not give a valid UTC time (bits; 0 = valid)
static constexpr uint32_t GNSS_UBX_INVALID_FIX = 0x01;           // fix type not 3, 4 or 5, or gnssFixOK not set
static constexpr uint32_t GNSS_UBX_INVALID_DATE_TIME = 0x02;     // validDate or validTime not set
static constexpr uint32_t GNSS_UBX_INVALID_NOT_RESOLVED = 0x04;  // not fully resolved (leap seconds not known)
static constexpr uint32_t GNSS_UBX_INVALID_NOT_CONFIRMED = 0x08; // confirmedAvai set, but confirmedTime not
static constexpr uint32_t GNSS_UBX_INVALID_SATELLITES = 0x10;    // fewer satellites than the minimum
static constexpr uint32_t GNSS_UBX_INVALID_UTC = 0x20;           // NAV-TIMEUTC of the epoch missing or validUTC not set

uint32_t gnss_ubx_pvt_invalid_reasons(const gnss_ubx_pvt_t &pvt, uint8_t min_satellites);

// NAV-TIMEUTC is configured: from now on its validUTC is required (gnss_ubx_pvt_invalid_reasons)
void gnss_ubx_set_timeutc_required(bool required);

// Quantization error (ps) of the time pulse at the given UTC second, from the TIM-TP that announced this pulse.
// false: no TIM-TP for this pulse, qErr flagged invalid, or the time base of the pulse is not usable.
bool gnss_ubx_qerr_for_edge(int64_t edge_unix_sec, int32_t *qerr_ps);

struct gnss_ubx_status_t
{
    uint32_t frames;
    uint32_t pvt_messages;
    uint32_t timeutc_messages;
    uint32_t tp_messages;
    uint32_t qerr_matched;   // pulses with a TIM-TP of the right time of week
    uint32_t qerr_unmatched; // pulses asked for without one
    uint32_t qerr_invalid;   // TIM-TP of the pulse flagged qErrInvalid
    uint32_t naks;
    int32_t last_qerr_ps;
    int16_t leap_seconds;    // GPS - UTC from NAV-PVT (-32768 = not known)
    uint8_t tp_time_base;    // of the latest TIM-TP: 0 GNSS, 1 UTC, 255 = none
};

void gnss_ubx_get_status(gnss_ubx_status_t *status);

// Debug log (reader task, once a minute); nothing before the first frame
void gnss_ubx_log_status(void);
