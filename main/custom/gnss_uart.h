// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- UART of the GNSS receiver (ESP-IDF driver) ----
// A task reads the UART (2 kB receive buffer) and splits the bytes into NMEA sentences ("$...\r\n", handed to
// custom/gnss_nmea) and UBX frames (sync B5 62, class, ID, length, payload, Fletcher checksum, handed to
// custom/gnss_ubx). Only the UBX functions write to the receiver; with the NMEA protocol nothing is sent.

struct gnss_uart_config_t
{
    int uart_num;
    int rx_pin;
    int tx_pin;
    uint32_t baud;
    uint32_t task_stack;
    unsigned int task_priority;
    int task_core;
    void (*loop_hook)(void); // called by the reader task once per pass (stack measurement); may be NULL
};

// Starts the reader (a running one is stopped first). Returns false if the UART or the task could not be set up.
bool gnss_uart_start(const gnss_uart_config_t &config);
void gnss_uart_stop(void);
bool gnss_uart_running(void);

// Changes the baud rate of the running UART (or starts it with the last configuration) and discards what was received
bool gnss_uart_set_baud(uint32_t baud);

// Discards everything received so far, also a sentence or frame whose reception has started. Returns the generation:
// a sentence or frame counts as received after the flush if its generation is at least this one.
uint32_t gnss_uart_flush(void);

// Sends bytes to the receiver (blocks until they are in the transmit FIFO). Returns false if the UART is not running.
bool gnss_uart_write(const uint8_t *data, size_t length);

// Received traffic since the last gnss_uart_reset_traffic() (baud rate detection)
struct gnss_uart_traffic_t
{
    uint32_t bytes;
    uint32_t nmea_sentences; // with a correct checksum
    uint32_t ubx_frames;     // with a correct checksum
    char sample[33];         // first bytes, printable (others as '.')
};

void gnss_uart_reset_traffic(void);
void gnss_uart_get_traffic(gnss_uart_traffic_t *traffic);

// Receive buffer or FIFO overflows (data lost) since the start
uint32_t gnss_uart_overflows(void);
