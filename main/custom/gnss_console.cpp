// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "gnss_console.h"

#include <algorithm>
#include <cstring>

namespace
{
    // Text building without printf: gnss_console_describe_ubx() runs in the GNSS reader task, whose stack is small.
    // Both keep text terminated and its length below size.
    void append(char *text, size_t size, size_t &used, const char *part, size_t length)
    {
        const size_t room = used + 1 < size ? size - 1 - used : 0;
        const size_t n = std::min(length, room);
        if (n > 0)
            memcpy(text + used, part, n);
        used += n;
        text[used] = '\0';
    }

    void append(char *text, size_t size, size_t &used, const char *part)
    {
        append(text, size, used, part, strlen(part));
    }

    void append_hex(char *text, size_t size, size_t &used, uint8_t value)
    {
        static const char digits[] = "0123456789ABCDEF";
        const char hex[2] = {digits[value >> 4], digits[value & 0x0F]};
        append(text, size, used, hex, 2);
    }

    void append_number(char *text, size_t size, size_t &used, unsigned value)
    {
        char digits[10];
        size_t n = 0;
        do
        {
            digits[sizeof(digits) - 1 - n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && n < sizeof(digits));
        append(text, size, used, digits + sizeof(digits) - n, n);
    }

    int hex_value(char ch)
    {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    }
}

void gnss_console_ring_add(gnss_console_ring_t &ring, uint8_t kind, bool periodic, const char *text)
{
    const uint32_t seq = ++ring.last_seq;
    gnss_console_entry_t &entry = ring.entries[seq % GNSS_CONSOLE_ENTRIES];
    entry.seq = seq;
    entry.kind = kind;
    entry.periodic = periodic;
    const size_t length = text != nullptr ? strnlen(text, sizeof(entry.text) - 1) : 0;
    if (length > 0)
        memcpy(entry.text, text, length);
    entry.text[length] = '\0';
}

bool gnss_console_ring_next(const gnss_console_ring_t &ring, uint32_t after, gnss_console_entry_t *entry)
{
    if (entry == nullptr || after >= ring.last_seq)
        return false;
    const uint32_t oldest = ring.last_seq >= GNSS_CONSOLE_ENTRIES ? ring.last_seq - GNSS_CONSOLE_ENTRIES + 1 : 1;
    const uint32_t seq = std::max(after + 1, oldest);
    *entry = ring.entries[seq % GNSS_CONSOLE_ENTRIES];
    return entry->seq == seq;
}

bool gnss_console_line_is_periodic(const char *line)
{
    // "$" + talker (2 characters) + sentence type (3) + ","
    if (line == nullptr || line[0] != '$' || strnlen(line, 7) < 7 || line[6] != ',')
        return false;
    static const char *const types[] = {"RMC", "GGA", "GLL", "GSA", "GSV", "VTG", "ZDA", "GNS", "GST", "GBS", "GRS", "DTM", "THS"};
    for (const char *type : types)
        if (memcmp(line + 3, type, 3) == 0)
            return true;
    return false;
}

bool gnss_console_ubx_is_periodic(uint8_t cls, uint8_t id)
{
    return cls == 0x01 || (cls == 0x0D && id == 0x01);
}

void gnss_console_describe_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length, char *text, size_t size)
{
    if (text == nullptr || size == 0)
        return;
    text[0] = '\0';
    if (payload == nullptr)
        length = 0;
    size_t used = 0;
    append(text, size, used, "UBX ");
    append_hex(text, size, used, cls);
    append(text, size, used, "-");
    append_hex(text, size, used, id);
    if (cls == 0x05 && (id == 0x00 || id == 0x01) && length >= 2)
    {
        append(text, size, used, id == 0x01 ? " ACK for " : " NAK for ");
        append_hex(text, size, used, payload[0]);
        append(text, size, used, "-");
        append_hex(text, size, used, payload[1]);
        return;
    }
    if (cls == 0x0A && id == 0x04 && length >= 40)
    {
        // MON-VER: software version (30 characters), hardware version (10), then extensions of 30 characters
        append(text, size, used, " MON-VER:");
        for (size_t offset = 0; offset < length;)
        {
            const size_t field = offset == 30 ? 10 : 30;
            if (offset + field > length)
                break;
            const char *start = reinterpret_cast<const char *>(payload + offset);
            const size_t chars = strnlen(start, field);
            bool printable = chars > 0;
            for (size_t i = 0; i < chars; ++i)
                printable = printable && start[i] >= 32 && start[i] <= 126;
            if (printable)
            {
                append(text, size, used, " ");
                append(text, size, used, start, chars);
            }
            offset += field;
        }
        return;
    }
    append(text, size, used, " (");
    append_number(text, size, used, length);
    append(text, size, used, length == 1 ? " byte)" : " bytes)");
    if (length > 0)
        append(text, size, used, ":");
    for (uint16_t i = 0; i < length; ++i)
    {
        // Room for this byte and for " ..." if more follow
        if (used + 3 + (i + 1u < length ? 4u : 0u) >= size)
        {
            append(text, size, used, " ...");
            return;
        }
        append(text, size, used, " ");
        append_hex(text, size, used, payload[i]);
    }
}

bool gnss_console_build_text(const char *command, bool checksum, bool crlf, uint8_t *out, size_t out_size, size_t *out_length,
                             const char **error)
{
    const char *unused = nullptr;
    const char *&reason = error != nullptr ? *error : unused;
    if (command == nullptr || out == nullptr || out_length == nullptr || out_size < GNSS_CONSOLE_COMMAND_MAX + 5)
    {
        reason = "Internal error";
        return false;
    }
    const size_t length = strnlen(command, GNSS_CONSOLE_COMMAND_MAX + 1);
    if (length == 0)
    {
        reason = "The command is empty";
        return false;
    }
    if (length > GNSS_CONSOLE_COMMAND_MAX)
    {
        reason = "The command is too long (at most 200 characters)";
        return false;
    }
    for (size_t i = 0; i < length; ++i)
    {
        if (command[i] < 32 || command[i] > 126)
        {
            reason = "Only printable ASCII characters are allowed";
            return false;
        }
    }
    if (checksum && memchr(command, '*', length) != nullptr)
    {
        reason = "The command has a '*' already: switch the checksum off or remove it";
        return false;
    }

    memcpy(out, command, length);
    size_t used = length;
    if (checksum)
    {
        uint8_t sum = 0;
        for (size_t i = (command[0] == '$' || command[0] == '!') ? 1 : 0; i < length; ++i)
            sum = static_cast<uint8_t>(sum ^ static_cast<uint8_t>(command[i]));
        static const char digits[] = "0123456789ABCDEF";
        out[used++] = '*';
        out[used++] = static_cast<uint8_t>(digits[sum >> 4]);
        out[used++] = static_cast<uint8_t>(digits[sum & 0x0F]);
    }
    if (crlf)
    {
        out[used++] = '\r';
        out[used++] = '\n';
    }
    *out_length = used;
    return true;
}

bool gnss_console_parse_ubx(const char *command, uint8_t *cls, uint8_t *id, uint8_t *payload, uint16_t *length,
                            const char **error)
{
    const char *unused = nullptr;
    const char *&reason = error != nullptr ? *error : unused;
    if (command == nullptr || cls == nullptr || id == nullptr || payload == nullptr || length == nullptr)
    {
        reason = "Internal error";
        return false;
    }
    if (strnlen(command, GNSS_CONSOLE_COMMAND_MAX * 2 + 1) > GNSS_CONSOLE_COMMAND_MAX * 2)
    {
        reason = "The command is too long";
        return false;
    }

    // Sync (2) + class, ID (2) + length (2) + payload + checksum (2)
    uint8_t bytes[8 + GNSS_CONSOLE_UBX_PAYLOAD_MAX];
    size_t count = 0;
    for (const char *p = command; *p != '\0';)
    {
        if (*p == ' ' || *p == ',' || *p == ':' || *p == '-' || *p == '\t')
        {
            ++p;
            continue;
        }
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && hex_value(p[2]) >= 0)
            p += 2;
        const int high = hex_value(p[0]);
        const int low = high >= 0 ? hex_value(p[1]) : -1;
        if (high < 0 || low < 0)
        {
            reason = "Hexadecimal bytes expected (two digits each), e.g. \"0A 04\"";
            return false;
        }
        if (count >= sizeof(bytes))
        {
            reason = "The command is too long (payload at most 100 bytes)";
            return false;
        }
        bytes[count++] = static_cast<uint8_t>((high << 4) | low);
        p += 2;
    }

    if (count >= 2 && bytes[0] == 0xB5 && bytes[1] == 0x62)
    {
        // A whole frame: its length and checksum have to be right
        if (count < 8)
        {
            reason = "A frame with B5 62 needs class, ID, length and checksum";
            return false;
        }
        const size_t frame_length = static_cast<size_t>(bytes[4]) | (static_cast<size_t>(bytes[5]) << 8);
        if (count != 8 + frame_length)
        {
            reason = "The length field does not match the payload";
            return false;
        }
        uint8_t ck_a = 0;
        uint8_t ck_b = 0;
        for (size_t i = 2; i < 6 + frame_length; ++i)
        {
            ck_a = static_cast<uint8_t>(ck_a + bytes[i]);
            ck_b = static_cast<uint8_t>(ck_b + ck_a);
        }
        if (ck_a != bytes[6 + frame_length] || ck_b != bytes[7 + frame_length])
        {
            reason = "Wrong checksum (or leave out B5 62, the length and the checksum: they are added)";
            return false;
        }
        *cls = bytes[2];
        *id = bytes[3];
        *length = static_cast<uint16_t>(frame_length);
        if (frame_length > 0)
            memcpy(payload, bytes + 6, frame_length);
        return true;
    }

    if (count < 2)
    {
        reason = "Class and ID expected, e.g. \"0A 04\" for MON-VER";
        return false;
    }
    if (count - 2 > GNSS_CONSOLE_UBX_PAYLOAD_MAX)
    {
        reason = "The command is too long (payload at most 100 bytes)";
        return false;
    }
    *cls = bytes[0];
    *id = bytes[1];
    *length = static_cast<uint16_t>(count - 2);
    if (count > 2)
        memcpy(payload, bytes + 2, count - 2);
    return true;
}
