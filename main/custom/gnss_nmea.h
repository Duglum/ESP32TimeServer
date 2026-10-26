// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Date and time (UTC) of an RMC sentence
struct nmea_rmc_time_t
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

// ---- Sentence checks ----

// "$...*hh": the two hexadecimal digits match the XOR of the characters between '$' and '*'
bool gnss_nmea_checksum_valid(const char *sentence);
// RMC with status A, an accepted mode indicator (field 12: A, D, P, R, F; a receiver without the field is judged by
// the status alone) and a plausible date and time
bool gnss_nmea_parse_rmc(const char *sentence, nmea_rmc_time_t *time_data);

// ---- Sentences of the GNSS receiver (NMEA protocol, and the NMEA output of a u-blox receiver) ----
// The reader task of custom/gnss_uart hands every sentence over. RMC gives the time; GGA is optional: while GGA
// sentences arrive, an RMC only counts if the latest GGA (at most 2 s old) reports a fix (quality 1 to 5), and GGA gives
// the satellites in use. GSV gives the satellites in view when there is no GGA.

struct gnss_nmea_config_t
{
    int64_t (*last_pps_us)(void);         // esp_timer time of the last PPS edge (0 = none), for the RMC phase; may be NULL
    void (*on_satellites)(uint8_t count); // satellites in use (GGA) or in view (GSV); may be NULL
};

// Sets the callbacks (before the UART is started)
void gnss_nmea_init(const gnss_nmea_config_t &config);

// Reader task of custom/gnss_uart: a complete sentence ("$...", without the line end) and the flush generation at its
// start. Returns false if the checksum is wrong.
bool gnss_nmea_process_sentence(const char *sentence, uint32_t generation);

// Waits until deadline_us (esp_timer) for a valid RMC. generation != 0: the sentence started after the flush
// (gnss_uart_flush) that returned this generation (it may have arrived before the call); generation == 0: the next
// valid RMC after the call.
bool gnss_nmea_wait_valid_rmc(uint32_t generation, int64_t deadline_us, nmea_rmc_time_t *time_data);

// Minimum satellites in use (0 = off): an RMC only counts if the latest GGA (at most 2 s old) reports at least this many
// satellites; without GGA no RMC counts while it is on. Takes effect at once.
void gnss_nmea_set_min_satellites(uint8_t count);

// The latest RMC was valid and is at most max_age_us old
bool gnss_nmea_time_valid(int64_t max_age_us);

static constexpr size_t GNSS_NMEA_RMC_PHASE_BINS = 10; // 0-99 ms, 100-199 ms, ... 900 ms and later

struct gnss_nmea_status_t
{
    bool running;
    bool gga_recent;          // a GGA sentence within the last 2 s
    uint8_t fix_quality;      // of that GGA
    uint8_t satellites_used;  // of that GGA
    int32_t rmc_after_pps_ms; // reception of the latest RMC after the last PPS edge, -1 = not known
    int32_t rmc_after_pps_min_ms;
    int32_t rmc_after_pps_max_ms;
    uint32_t sentences;
    uint32_t checksum_errors;
    uint32_t overflows;       // receive buffer or FIFO overflows (data lost)
    uint32_t rmc_phase_bins[GNSS_NMEA_RMC_PHASE_BINS]; // RMC receptions since the start per 100 ms after the PPS edge
};

void gnss_nmea_get_status(gnss_nmea_status_t *status);

// Debug log (reader task, once a minute): RMC phase and counters; nothing before the first sentence
void gnss_nmea_log_status(void);
