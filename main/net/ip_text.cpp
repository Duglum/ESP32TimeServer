// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// IPv4 and IPv6 addresses as text, see ip_text.h

#include "ip_text.h"

#include <cstdio>
#include <cstring>

namespace
{
    int hex_value(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    // Dotted decimal from p up to end (exclusive)
    bool parse_ipv4_range(const char *p, const char *end, uint8_t out[4])
    {
        for (int part = 0; part < 4; ++part)
        {
            if (part > 0)
            {
                if (p >= end || *p != '.')
                    return false;
                ++p;
            }
            int value = 0;
            int digits = 0;
            const char *start = p;
            while (p < end && *p >= '0' && *p <= '9')
            {
                value = value * 10 + (*p - '0');
                ++p;
                if (++digits > 3)
                    return false;
            }
            if (digits == 0 || value > 255 || (digits > 1 && *start == '0'))
                return false;
            out[part] = static_cast<uint8_t>(value);
        }
        return p == end;
    }
} // namespace

bool ip_parse_ipv4(const char *text, uint8_t out[4])
{
    if (text == nullptr)
        return false;
    const size_t length = strnlen(text, IP_TEXT_V4_SIZE);
    if (length == 0 || length >= IP_TEXT_V4_SIZE)
        return false;
    uint8_t parsed[4];
    if (!parse_ipv4_range(text, text + length, parsed))
        return false;
    if (out != nullptr)
        memcpy(out, parsed, 4);
    return true;
}

bool ip_parse_ipv6(const char *text, uint8_t out[16])
{
    if (text == nullptr)
        return false;
    const size_t length = strnlen(text, IP_TEXT_V6_SIZE);
    if (length == 0 || length >= IP_TEXT_V6_SIZE)
        return false;
    const char *p = text;
    const char *const end = text + length;

    uint16_t words[8] = {};
    int count = 0; // groups parsed
    int gap = -1;  // position of "::" in groups, -1 = none
    if (p[0] == ':')
    {
        if (length < 2 || p[1] != ':')
            return false;
        gap = 0;
        p += 2;
    }
    while (p < end)
    {
        // An embedded IPv4 address: the rest has a '.' and no ':'
        const char *colon = static_cast<const char *>(memchr(p, ':', static_cast<size_t>(end - p)));
        if (colon == nullptr && memchr(p, '.', static_cast<size_t>(end - p)) != nullptr)
        {
            uint8_t v4[4];
            if (count > 6 || !parse_ipv4_range(p, end, v4))
                return false;
            words[count++] = static_cast<uint16_t>((v4[0] << 8) | v4[1]);
            words[count++] = static_cast<uint16_t>((v4[2] << 8) | v4[3]);
            p = end;
            break;
        }
        if (count == 8)
            return false;
        int value = 0;
        int digits = 0;
        while (p < end && hex_value(*p) >= 0)
        {
            value = (value << 4) | hex_value(*p);
            ++p;
            if (++digits > 4)
                return false;
        }
        if (digits == 0)
            return false;
        words[count++] = static_cast<uint16_t>(value);
        if (p == end)
            break;
        if (*p != ':')
            return false;
        ++p;
        if (p < end && *p == ':')
        {
            if (gap >= 0)
                return false; // a second "::"
            gap = count;
            ++p;
        }
        else if (p == end)
            return false; // a single ':' at the end
    }

    uint16_t full[8] = {};
    if (gap < 0)
    {
        if (count != 8)
            return false;
        memcpy(full, words, sizeof(full));
    }
    else
    {
        if (count > 7) // "::" stands for at least one group
            return false;
        for (int i = 0; i < gap; ++i)
            full[i] = words[i];
        const int tail = count - gap;
        for (int i = 0; i < tail; ++i)
            full[8 - tail + i] = words[gap + i];
    }
    if (out != nullptr)
        for (int i = 0; i < 8; ++i)
        {
            out[2 * i] = static_cast<uint8_t>(full[i] >> 8);
            out[2 * i + 1] = static_cast<uint8_t>(full[i] & 0xFF);
        }
    return true;
}

void ip_format_ipv4(const uint8_t address[4], char *out, size_t size)
{
    if (out == nullptr || size == 0)
        return;
    if (size < IP_TEXT_V4_SIZE)
    {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
}

void ip_format_ipv6(const uint8_t address[16], char *out, size_t size)
{
    if (out == nullptr || size == 0)
        return;
    out[0] = '\0';
    if (size < IP_TEXT_V6_SIZE)
        return;
    static constexpr uint8_t v4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (memcmp(address, v4_mapped, 12) == 0)
    {
        snprintf(out, size, "::ffff:%u.%u.%u.%u", address[12], address[13], address[14], address[15]);
        return;
    }
    uint16_t words[8];
    for (int i = 0; i < 8; ++i)
        words[i] = static_cast<uint16_t>((address[2 * i] << 8) | address[2 * i + 1]);
    int best_start = -1;
    int best_length = 0;
    for (int i = 0; i < 8;)
    {
        if (words[i] != 0)
        {
            ++i;
            continue;
        }
        int j = i;
        while (j < 8 && words[j] == 0)
            ++j;
        if (j - i > best_length)
        {
            best_start = i;
            best_length = j - i;
        }
        i = j;
    }
    if (best_length < 2)
        best_start = -1;

    size_t used = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (i == best_start)
        {
            used += static_cast<size_t>(snprintf(out + used, size - used, "::"));
            i += best_length - 1;
            continue;
        }
        const bool after_gap = best_start >= 0 && i == best_start + best_length;
        used += static_cast<size_t>(snprintf(out + used, size - used, (i == 0 || after_gap) ? "%x" : ":%x", words[i]));
    }
}
