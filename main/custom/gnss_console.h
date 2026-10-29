// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- GNSS console of the web page: commands and the record of the traffic ----
// No FreeRTOS here (host tested): gnss_uart keeps the ring and locks it, the web server builds the commands.

static constexpr size_t GNSS_CONSOLE_ENTRIES = 48;
static constexpr size_t GNSS_CONSOLE_TEXT_SIZE = 164;
static constexpr size_t GNSS_CONSOLE_COMMAND_MAX = 200;     // characters of a typed command
static constexpr size_t GNSS_CONSOLE_UBX_PAYLOAD_MAX = 100; // payload of a UBX command (send buffer of gnss_ubx)

enum gnss_console_kind_t : uint8_t
{
    GNSS_CONSOLE_SENT = 0, // a command of the web page
    GNSS_CONSOLE_TEXT = 1, // a received text line
    GNSS_CONSOLE_UBX = 2   // a received UBX frame (summary)
};

struct gnss_console_entry_t
{
    uint32_t seq;  // 1, 2, ... in the order of the entries
    uint8_t kind;  // gnss_console_kind_t
    bool periodic; // a periodic message (standard NMEA sentence such as RMC or GSV, UBX NAV or TIM-TP)
    char text[GNSS_CONSOLE_TEXT_SIZE];
};

// The last GNSS_CONSOLE_ENTRIES entries; zero-initialised = empty
struct gnss_console_ring_t
{
    gnss_console_entry_t entries[GNSS_CONSOLE_ENTRIES];
    uint32_t last_seq; // 0 = no entry yet
};

// Adds an entry (the text is cut to GNSS_CONSOLE_TEXT_SIZE - 1 characters)
void gnss_console_ring_add(gnss_console_ring_t &ring, uint8_t kind, bool periodic, const char *text);
// The oldest entry kept with a sequence number above after; false if there is none
bool gnss_console_ring_next(const gnss_console_ring_t &ring, uint32_t after, gnss_console_entry_t *entry);

// A standard NMEA sentence that receivers send every second ("$ttRMC,", GGA, GSV, ...)
bool gnss_console_line_is_periodic(const char *line);
// A UBX frame that comes every second (NAV class, TIM-TP)
bool gnss_console_ubx_is_periodic(uint8_t cls, uint8_t id);

// Summary of a received UBX frame for the console: class and ID, ACK / NAK with the message, the version strings of
// MON-VER, otherwise the payload in hexadecimal (cut with "..." when it does not fit)
void gnss_console_describe_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length, char *text, size_t size);

// Text command: 1 to GNSS_CONSOLE_COMMAND_MAX printable ASCII characters. checksum: appends "*hh", the XOR of the
// characters after a leading '$' or '!' (of all characters without it); refused if the command has a '*' already.
// crlf: appends CR LF. out needs GNSS_CONSOLE_COMMAND_MAX + 5 bytes. On error, *error names the reason.
bool gnss_console_build_text(const char *command, bool checksum, bool crlf, uint8_t *out, size_t out_size, size_t *out_length,
                             const char **error);

// UBX command in hexadecimal: "<class> <ID> <payload ...>" (the firmware adds sync, length and checksum), or a whole
// frame "B5 62 <class> <ID> <length> <payload> <checksum>" whose length and checksum are checked. Bytes may be
// separated by spaces, commas, colons or dashes, with or without "0x". payload needs GNSS_CONSOLE_UBX_PAYLOAD_MAX bytes.
bool gnss_console_parse_ubx(const char *command, uint8_t *cls, uint8_t *id, uint8_t *payload, uint16_t *length,
                            const char **error);
