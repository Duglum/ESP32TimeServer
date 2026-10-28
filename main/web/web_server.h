// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Web admin page: HTTPS on TCP 443 only (IPv4 and IPv6), self-signed per-device certificate.

#pragma once

#include <cstddef>
#include <cstdint>

#include "web_settings.h"

typedef struct esp_netif_obj esp_netif_t;

struct web_app_status_t
{
    char firmware_version[24];
    char build[48];
    bool firmware_check_pending;  // a new firmware of an update is still being checked (review E-06)
    char chip_revision[8];        // e.g. "v3.2"
    bool ptp_loopback_supported;  // chip revision 3.0 or newer (PTP PPS loopback)
    bool time_set;
    bool pps_active;
    bool gnss_locked;
    bool stratum_one;
    bool pps_holdover;            // stratum 1 in the PPS holdover (PPS lost)
    bool gnss_fix_waiting;        // no stratum 1 yet: the minimum GNSS fix age after the start is not reached
    uint32_t gnss_fix_age_s;      // age of the current GNSS fix (0 = none)
    bool ublox_receiver;          // UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED: the baud rate is detected automatically
    uint32_t gnss_baud_in_use;
    char gnss_dynamic_model[16];  // U-Blox UBX: dynamic platform model read from the receiver ("" = not known)
    int16_t gnss_satellites_used; // NMEA: satellites in use of the latest GGA (-1 = no GGA)
    int16_t gnss_fix_quality;     // NMEA: fix quality of the latest GGA (-1 = no GGA)
    int32_t gnss_rmc_after_pps_ms; // NMEA: reception of the latest RMC after the PPS edge (-1 = not known)
    int32_t gnss_rmc_after_pps_min_ms;
    int32_t gnss_rmc_after_pps_max_ms;
    int16_t gnss_pvt_satellites;  // U-Blox UBX: satellites used in the latest NAV-PVT (-1 = none)
    uint8_t gnss_qerr_state;      // U-Blox UBX: qErr correction 0 no qErr, 1 learning the sign, 2 active, 3 no correlation,
                                  // 4 the receiver flags every qErr invalid (TIM-TP qErrInvalid)
    int32_t gnss_qerr_slope_milli; // regression slope of the qErr correction x 1000 (states 2 and 3)
    bool gnss_rf_known;           // U-Blox UBX: MON-RF answered within the last 30 s
    uint8_t gnss_antenna_status;  // 0 starting, 1 unknown, 2 OK, 3 short circuit, 4 open
    uint8_t gnss_antenna_power;   // 0 off, 1 on, 2 unknown
    uint8_t gnss_jamming_state;   // 0 unknown, 1 OK, 2 warning, 3 critical
    uint8_t gnss_agc_percent;     // AGC monitor, 0 to 100 %
    bool lcd_present;
    bool led_active;
    bool mqtt_connected;
    bool mqtt_tf_queue;           // MQTT reports are queued on the TF card (QoS 1 or 2 and a usable FAT32 card)
    char mac_address[WEB_MAC_TEXT_SIZE]; // MAC in use since the start
    bool ntp_auth_available;
    size_t ntp_auth_compiled_keys;
    size_t ntp_auth_keys_in_use;
    uint32_t ntp_rate_limited_requests; // NTP requests dropped by the per-client rate limit since the start
    uint32_t free_heap;
};

// Diagnostics of the time keeping (review 4.5) for the collapsed Diagnostics panel. Read on request only
// (GET /api/diag); the stability (ADEV, TDEV) is computed in the web server task.
static constexpr size_t WEB_DIAG_TAUS = 11;     // tau = 1, 2, 4, ..., 1024 s
static constexpr size_t WEB_DIAG_ISR_BINS = 6;  // excess up to 0.25, 0.5, 1, 2, 5 us and above
static constexpr size_t WEB_DIAG_RMC_BINS = 10; // 100 ms each within the first second after the PPS edge
static constexpr size_t WEB_DIAG_TX_PHASE_BINS = 10; // 100 ms each of the second of the basic mode T3
static constexpr size_t WEB_DIAG_TASKS = 12;         // the watched tasks of the firmware and the web admin task

struct web_diag_task_t
{
    const char *name;    // a string literal
    uint32_t stack_size; // bytes
    uint32_t least_free; // bytes never used since the task started (stack high water mark)
};

struct web_diag_t
{
    // Servo of the free-running PTP clock
    bool servo;                   // the firmware has it (PTP_FREE_RUNNING)
    bool servo_valid;             // the filter has an estimate
    bool loopback_active;         // PTP PPS loopback set up
    bool from_loopback;           // the last measurement came from the loopback
    float offset_ns;              // estimated offset of the raw PTP clock (raw - true)
    float noise_ns;               // running mean of |innovation|
    float free_ppb;               // rate of the PTP clock without frequency command
    int32_t cmd_ppb;              // frequency command
    float sw_bias_ns;             // software - loopback measurement
    uint32_t sw_bias_samples;
    float relock_residual_ns;     // mapping residual after a holdover
    uint32_t loopback_used;
    uint32_t loopback_missing;
    uint32_t loopback_rejected;
    uint32_t outliers;
    uint32_t steps;
    uint32_t command_failures;
    // PPS holdover: running one, last one ended (duration, time error measured at the re-lock)
    int32_t holdover_running_s;   // seconds of the running holdover (-1 = none)
    uint32_t holdovers;           // ended since the start
    int64_t last_holdover_end_s;  // uptime at the end of the last one (-1 = none)
    float last_holdover_s;
    float last_holdover_error_ns; // served time - true time when the PPS came back
    bool last_holdover_stepped;   // more than 100 us off: the time was set instead of ramped
    // RMS of the innovations (normal filter updates)
    float rms_minute_ns;
    uint32_t rms_minute_samples;
    float rms_hour_ns;
    uint32_t rms_hour_samples;
    uint32_t rms_age_s;           // seconds since the last complete minute (UINT32_MAX = none)
    // Stability of the GNSS PPS against the free-running PTP clock
    uint32_t stability_samples;   // seconds of continuous data
    uint8_t stability_source;     // 1 loopback, 2 software readings
    uint8_t stability_count;
    uint16_t tau_s[WEB_DIAG_TAUS];
    float adev[WEB_DIAG_TAUS];
    float tdev_ns[WEB_DIAG_TAUS]; // 0 = not enough data
    // PPS interrupt: latency in use and the excess of the interrupt time over its central value
    float isr_latency_us;
    uint32_t isr_bins[WEB_DIAG_ISR_BINS];
    uint32_t isr_total;
    uint32_t isr_max_ns;
    // qErr (U-Blox UBX protocol)
    bool ublox;
    int8_t qerr_sign;
    uint32_t qerr_samples;
    float qerr_sd_ns;
    uint32_t qerr_matched;
    uint32_t qerr_unmatched;
    uint32_t qerr_invalid;
    uint32_t tp_messages;         // TIM-TP received
    int32_t tp_last_qerr_ps;      // qErr of the latest TIM-TP as received
    uint8_t tp_last_flags;
    // NTP paths since the start
    uint32_t hw_replies;
    uint32_t hw_interleaved;
    uint32_t hw_no_tx_stamp;
    uint32_t sw_replies;
    uint32_t sw_interleaved;
    uint32_t shed;
    uint32_t lookup_record_hits;
    uint32_t lookup_map_hits;
    uint32_t lookup_misses;
    bool tx_delay;                // the basic mode T3 is compensated by the learned TX delay
    int32_t tx_delay_v4_ns;
    uint32_t tx_delay_v4_samples;
    int32_t tx_delay_v6_ns;
    uint32_t tx_delay_v6_samples;
    // TX delay of unauthenticated replies, [0] IPv4, [1] IPv6: minimum and maximum since the start (0 = no value),
    // running mean and count per 100 ms phase of the second of the software T3
    int32_t tx_delay_min_ns[2];
    int32_t tx_delay_max_ns[2];
    int32_t tx_delay_phase_ns[2][WEB_DIAG_TX_PHASE_BINS];
    uint32_t tx_delay_phase_samples[2][WEB_DIAG_TX_PHASE_BINS];
    // GNSS receiver
    uint32_t uart_overflows;
    uint32_t nmea_sentences;
    uint32_t nmea_checksum_errors;
    uint32_t rmc_phase_bins[WEB_DIAG_RMC_BINS];
    uint32_t ubx_frames;
    uint32_t ubx_naks;
    int64_t first_resolved_s;     // uptime when NAV-PVT fullyResolved was first set (-1 = not yet)
    uint32_t resolved_losses;
    int64_t first_valid_utc_s;    // the same for NAV-TIMEUTC validUTC
    uint32_t valid_utc_losses;
    // Time pulse 1 of the U-Blox receiver (CFG-TP, read at the setup)
    bool tp_known;
    bool tp_enabled;
    bool tp_frequency_mode;       // period values in Hz instead of us
    bool tp_length_mode;          // pulse length in us (false: a ratio, not read)
    uint32_t tp_period;           // without lock
    uint32_t tp_period_lock;
    uint32_t tp_length_us;        // without lock (0 = no pulse without lock)
    uint32_t tp_length_lock_us;
    bool tp_rising;               // rising edge at the top of the second
    bool tp_aligned;              // ALIGN_TO_TOW, SYNC_GNSS and USE_LOCKED set
    uint8_t tp_time_grid;         // 0 UTC, 1 GPS, 2 GLONASS, 3 BeiDou, 4 Galileo
    int32_t tp_cable_delay_ns;
    int32_t tp_user_delay_ns;
    // RF of the U-Blox receiver (MON-RF, within the last 30 s)
    bool rf_known;
    uint16_t rf_noise_per_ms;
    uint16_t rf_agc_count;        // 0 to 8191
    uint8_t rf_jam_indicator;     // CW jamming indicator, 0 to 255
    // Device: stack reserve of the tasks that noted it, and the internal heap
    web_diag_task_t tasks[WEB_DIAG_TASKS];
    uint8_t task_count;
    uint32_t heap_free;
    uint32_t heap_min_free;       // lowest free size since the start
    uint32_t heap_largest_block;
};

// Starts the web admin in its own task (TLS identity, password hash, HTTPS server). Call once the Ethernet
// interface exists. If the settings of this boot came from a one-shot set that changed the network, the 120 s
// confirmation starts as well.
void web_admin_start();
// The HTTPS server was started (false with WEB_ADMIN_ENABLED 0)
bool web_admin_running();

// Implemented in main.cpp
void app_get_status(web_app_status_t &status);
void app_get_diag(web_diag_t &diag);
esp_netif_t *app_eth_netif();
// The network configuration of the interface changed: refresh the address cache used by the NTP and LCD code
void app_network_settings_changed();
// Over The Ethernet update (firmware upload, see web_ote.h): switched on for this boot, upload progress, failure
// (reason shown on the LCD, at most 20 characters) and an accepted image (the device restarts a few seconds later)
bool app_ote_enabled();
void app_ote_progress(size_t written, size_t total);
void app_ote_failed(const char *reason);
void app_ote_accepted();
