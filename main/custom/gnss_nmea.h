// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include <stdbool.h>
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

// ---- Sentence checks (also used for the NMEA output of a u-blox receiver read through the u-blox library) ----

// "$...*hh": the two hexadecimal digits match the XOR of the characters between '$' and '*'
bool gnss_nmea_checksum_valid(const char *sentence);
// RMC with status A, an accepted mode indicator (field 12: A, D, P, R, F; a receiver without the field is judged by
// the status alone) and a plausible date and time
bool gnss_nmea_parse_rmc(const char *sentence, nmea_rmc_time_t *time_data);

// ---- Reader of a receiver with the NMEA protocol on an ESP-IDF UART ----
// A task reads the UART (ESP-IDF driver, 2 kB receive buffer) and parses the sentences byte by byte. Nothing is sent
// to the receiver. RMC gives the time; GGA is optional: while GGA sentences arrive, an RMC only counts if the latest
// GGA (at most 2 s old) reports a fix (quality 1 to 5), and GGA gives the satellites in use. GSV gives the satellites
// in view when there is no GGA.

struct gnss_nmea_config_t
{
    int uart_num;
    int rx_pin;
    int tx_pin;
    uint32_t baud;
    uint32_t task_stack;
    unsigned int task_priority;
    int task_core;
    int64_t (*last_pps_us)(void);         // esp_timer time of the last PPS edge (0 = none), for the RMC phase; may be NULL
    void (*on_satellites)(uint8_t count); // satellites in use (GGA) or in view (GSV); may be NULL
    void (*loop_hook)(void);              // called by the reader task once per pass (stack measurement); may be NULL
};

// Starts the reader (a running one is stopped first). Returns false if the UART or the task could not be set up.
bool gnss_nmea_start(const gnss_nmea_config_t &config);
void gnss_nmea_stop(void);

// Discards everything received so far, also a sentence whose reception has started. Returns the generation that
// gnss_nmea_wait_valid_rmc() needs to accept only sentences that started after this call.
uint32_t gnss_nmea_flush(void);

// Waits until deadline_us (esp_timer) for a valid RMC. generation != 0: the sentence started after the flush that
// returned this generation (it may have arrived before the call); generation == 0: the next valid RMC after the call.
bool gnss_nmea_wait_valid_rmc(uint32_t generation, int64_t deadline_us, nmea_rmc_time_t *time_data);

// Minimum satellites in use (0 = off): an RMC only counts if the latest GGA (at most 2 s old) reports at least this many
// satellites; without GGA no RMC counts while it is on. Takes effect at once.
void gnss_nmea_set_min_satellites(uint8_t count);

// The latest RMC was valid and is at most max_age_us old
bool gnss_nmea_time_valid(int64_t max_age_us);

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
};

void gnss_nmea_get_status(gnss_nmea_status_t *status);
