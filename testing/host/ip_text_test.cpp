// ESP32 Time Server host tests (from ESP32-ConsoleServer): IPv4 and IPv6 text forms (main/net/ip_text.cpp)
#define TEST_NAME "ip_text_test"
#include "check.h"

#include "net/ip_text.h"

#include <random>

namespace
{
    bool v4(const char *text, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
    {
        uint8_t out[4];
        return ip_parse_ipv4(text, out) && out[0] == a && out[1] == b && out[2] == c && out[3] == d;
    }

    // Parses text and returns the canonical form ("" if refused)
    const char *canonical(const char *text)
    {
        static char out[IP_TEXT_V6_SIZE];
        uint8_t address[16];
        if (!ip_parse_ipv6(text, address))
            return "";
        ip_format_ipv6(address, out, sizeof(out));
        return out;
    }

    // Round trip: format, parse, the same bytes; the text has at most 45 characters
    void check_round_trip(const uint8_t address[16])
    {
        char text[IP_TEXT_V6_SIZE];
        ip_format_ipv6(address, text, sizeof(text));
        CHECK(strlen(text) > 0 && strlen(text) < IP_TEXT_V6_SIZE);
        uint8_t again[16];
        CHECK(ip_parse_ipv6(text, again));
        CHECK(memcmp(address, again, 16) == 0);
    }
} // namespace

void run_tests()
{
    // ---- IPv4 ----
    CHECK(v4("0.0.0.0", 0, 0, 0, 0));
    CHECK(v4("192.168.1.10", 192, 168, 1, 10));
    CHECK(v4("255.255.255.255", 255, 255, 255, 255));
    CHECK(v4("10.0.0.1", 10, 0, 0, 1));
    for (const char *bad : {"", "1", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.256", "01.2.3.4", "1.2.3.04", "1..2.3",
                            ".1.2.3", "1.2.3.", " 1.2.3.4", "1.2.3.4 ", "1.2.3.4a", "a.b.c.d", "1.2.3.-4", "1.2.3.+4",
                            "0x1.2.3.4", "1234.1.1.1", "1.2.3.4/24", "999.999.999.999", "192.168.001.001"})
        CHECK(!ip_parse_ipv4(bad, nullptr));
    CHECK(!ip_parse_ipv4(nullptr, nullptr));
    char text[IP_TEXT_V4_SIZE];
    const uint8_t a4[4] = {192, 0, 2, 255};
    ip_format_ipv4(a4, text, sizeof(text));
    CHECK_STR(text, "192.0.2.255");
    char small[8] = "x";
    ip_format_ipv4(a4, small, sizeof(small));
    CHECK_STR(small, "");

    // ---- IPv6: accepted forms and their canonical form (RFC 5952) ----
    CHECK_STR(canonical("::"), "::");
    CHECK_STR(canonical("::1"), "::1");
    CHECK_STR(canonical("1::"), "1::");
    CHECK_STR(canonical("2001:DB8::1"), "2001:db8::1");
    CHECK_STR(canonical("2001:0db8:0000:0000:0000:0000:0000:0001"), "2001:db8::1");
    CHECK_STR(canonical("2001:db8:0:0:1:0:0:1"), "2001:db8::1:0:0:1");      // the first of two equal runs
    CHECK_STR(canonical("2001:0:0:1:0:0:0:1"), "2001:0:0:1::1");             // the longer run
    CHECK_STR(canonical("2001:db8:0:1:1:1:1:1"), "2001:db8:0:1:1:1:1:1");    // one zero group is not compressed
    CHECK_STR(canonical("fe80::1:2:3:4"), "fe80::1:2:3:4");
    CHECK_STR(canonical("1:2:3:4:5:6:7:8"), "1:2:3:4:5:6:7:8");
    CHECK_STR(canonical("1:2:3:4:5:6:7::"), "1:2:3:4:5:6:7:0");
    CHECK_STR(canonical("::2:3:4:5:6:7:8"), "0:2:3:4:5:6:7:8");
    CHECK_STR(canonical("::ffff:192.0.2.1"), "::ffff:192.0.2.1");
    CHECK_STR(canonical("::FFFF:C000:0201"), "::ffff:192.0.2.1");
    CHECK_STR(canonical("64:ff9b::192.0.2.33"), "64:ff9b::c000:221");
    CHECK_STR(canonical("1:2:3:4:5:6:1.2.3.4"), "1:2:3:4:5:6:102:304");
    CHECK_STR(canonical("fd00::"), "fd00::");
    CHECK_STR(canonical("0:0:0:0:0:0:0:0"), "::");
    // Refused forms
    for (const char *bad : {"", ":", ":::", "1:::2", "1::2::3", "12345::", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", ":1",
                            "1:", "1:2:", "g::", "1::2.3.4", "::1.2.3.4.5", "1:2:3:4:5:6:7::8", "::1:2:3:4:5:6:7:8",
                            "1:2:3:4:5:6:7:1.2.3.4", "1.2.3.4", "::01.2.3.4", "fe80::1%eth0", "[::1]", " ::1", "::1 ",
                            "2001:db8::1/64", "::1.2.3.4:5", "0000:0000:0000:0000:0000:0000:0000:0000:0", "::fffff",
                            "1:2:3:4:5:6:7:8::", "::1.2.3"})
    {
        if (ip_parse_ipv6(bad, nullptr))
            fprintf(stderr, "accepted: \"%s\"\n", bad);
        CHECK(!ip_parse_ipv6(bad, nullptr));
    }
    CHECK(!ip_parse_ipv6(nullptr, nullptr));
    // At most 45 characters (the longest form, the IPv4-mapped one with every digit, has 45)
    CHECK(ip_parse_ipv6("0000:0000:0000:0000:0000:ffff:255.255.255.255", nullptr));
    CHECK(!ip_parse_ipv6("00000:000:0000:0000:0000:ffff:255.255.255.255", nullptr));
    char too_small[IP_TEXT_V6_SIZE - 1] = "x";
    const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    ip_format_ipv6(loopback, too_small, sizeof(too_small));
    CHECK_STR(too_small, "");

    // ---- Round trips: random addresses and addresses with many zero groups ----
    std::mt19937 random(12345);
    for (int i = 0; i < 20000; ++i)
    {
        uint8_t address[16];
        for (int b = 0; b < 16; b += 2)
        {
            const bool zero = (random() % 3) == 0;
            address[b] = zero ? 0 : static_cast<uint8_t>(random());
            address[b + 1] = zero ? 0 : static_cast<uint8_t>(random());
        }
        if (i % 7 == 0) // IPv4-mapped
        {
            memset(address, 0, 10);
            address[10] = address[11] = 0xFF;
        }
        check_round_trip(address);
    }
}

void fuzz_one(const uint8_t *data, size_t size)
{
    char text[64];
    const size_t length = size < sizeof(text) - 1 ? size : sizeof(text) - 1;
    memcpy(text, data, length);
    text[length] = '\0';

    uint8_t v4_bytes[4];
    if (ip_parse_ipv4(text, v4_bytes))
    {
        char out[IP_TEXT_V4_SIZE];
        ip_format_ipv4(v4_bytes, out, sizeof(out));
        CHECK(strcmp(out, text) == 0); // the strict parser accepts only the canonical form
    }
    uint8_t v6_bytes[16];
    if (ip_parse_ipv6(text, v6_bytes))
    {
        CHECK(strlen(text) < IP_TEXT_V6_SIZE);
        check_round_trip(v6_bytes);
    }
    // Arbitrary bytes as an address
    if (size >= 16)
        check_round_trip(data);
}
