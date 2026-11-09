// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// IPv4 and IPv6 addresses as text: strict parsers and the canonical text forms (no lwIP, host tested and fuzzed).

#pragma once

#include <cstddef>
#include <cstdint>

static constexpr size_t IP_TEXT_V4_SIZE = 16; // "255.255.255.255" + NUL
static constexpr size_t IP_TEXT_V6_SIZE = 46; // INET6_ADDRSTRLEN

// Dotted decimal: exactly 4 parts of 0 to 255 without leading zeros ("010" is refused). out: network order.
bool ip_parse_ipv4(const char *text, uint8_t out[4]);

// RFC 4291 text form: 8 groups of 1 to 4 hex digits, one "::" for one or more zero groups, an embedded IPv4 address
// as the last 32 bits. No zone ("%eth0"), no brackets, at most 45 characters. out: network order.
bool ip_parse_ipv6(const char *text, uint8_t out[16]);

// Canonical forms; size at least IP_TEXT_V4_SIZE / IP_TEXT_V6_SIZE (otherwise out is set to "")
void ip_format_ipv4(const uint8_t address[4], char *out, size_t size);
// RFC 5952: lower case, no leading zeros, the longest run of two or more zero groups (the first of equal ones) as
// "::", IPv4-mapped addresses as ::ffff:a.b.c.d
void ip_format_ipv6(const uint8_t address[16], char *out, size_t size);
