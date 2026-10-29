// ESP32 Time Server - host tests
// GNSS console of the web page: the commands (main/custom/gnss_console.cpp), the ring of the recorded traffic and
// the recording by the UART reader (main/custom/gnss_uart.cpp), the raw UBX send (main/custom/gnss_ubx.cpp).
// Built twice by run_tests.ps1: as a test program (main) and, with HOST_FUZZ, as a libFuzzer target.

#include "gnss_console.h"
#include "gnss_nmea.h"
#include "gnss_uart.h"
#include "gnss_ubx.h"
#include "host_hooks.h"
#include "host_stubs.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using bytes_t = std::vector<uint8_t>;

    bytes_t s_written; // what was written to the UART

    void on_tx(const uint8_t *data, size_t length)
    {
        s_written.insert(s_written.end(), data, data + length);
    }

    void start()
    {
        host_uart_tx_hook = on_tx;
        s_written.clear();
        host_gnss_uart_reset(true);
        gnss_nmea_init(gnss_nmea_config_t{});
        gnss_ubx_init(gnss_ubx_config_t{});
    }

    std::string build(const char *command, bool checksum, bool crlf, bool *ok = nullptr, const char **error = nullptr)
    {
        uint8_t out[GNSS_CONSOLE_COMMAND_MAX + 5];
        size_t length = 0;
        const char *reason = nullptr;
        const bool built = gnss_console_build_text(command, checksum, crlf, out, sizeof(out), &length, &reason);
        if (ok != nullptr)
            *ok = built;
        if (error != nullptr)
            *error = reason;
        return built ? std::string(reinterpret_cast<const char *>(out), length) : std::string();
    }

    std::string describe(uint8_t cls, uint8_t id, const bytes_t &payload, size_t size = GNSS_CONSOLE_TEXT_SIZE)
    {
        std::vector<char> text(size + 1, 'X');
        gnss_console_describe_ubx(cls, id, payload.data(), static_cast<uint16_t>(payload.size()), text.data(), size);
        CHECK(text[size] == 'X');             // nothing written after the buffer
        CHECK(strnlen(text.data(), size) < size); // terminated within it
        return std::string(text.data());
    }

    bytes_t ubx_frame(uint8_t cls, uint8_t id, const bytes_t &payload)
    {
        bytes_t frame = {0xB5, 0x62, cls, id, static_cast<uint8_t>(payload.size()), static_cast<uint8_t>(payload.size() >> 8)};
        frame.insert(frame.end(), payload.begin(), payload.end());
        uint8_t a = 0;
        uint8_t b = 0;
        for (size_t i = 2; i < frame.size(); ++i)
        {
            a = static_cast<uint8_t>(a + frame[i]);
            b = static_cast<uint8_t>(b + a);
        }
        frame.push_back(a);
        frame.push_back(b);
        return frame;
    }

    void feed(const bytes_t &bytes)
    {
        host_gnss_uart_feed(bytes.data(), bytes.size(), 1);
    }

    void feed(const std::string &text)
    {
        host_gnss_uart_feed(reinterpret_cast<const uint8_t *>(text.data()), text.size(), 1);
    }

    std::vector<gnss_console_entry_t> entries_after(uint32_t after)
    {
        std::vector<gnss_console_entry_t> list;
        gnss_console_entry_t entry{};
        while (gnss_uart_console_next(after, &entry))
        {
            list.push_back(entry);
            after = entry.seq;
        }
        return list;
    }

    // ---- Tests ----
    void test_build_text()
    {
        // Checksum: XOR after the '$' ($GPGGA example of NMEA: "GPGLL,..." style), upper case hex, CR LF
        CHECK(build("$CFGMSG,0,4,1", true, true) == "$CFGMSG,0,4,1*" + std::string([] {
                  uint8_t x = 0;
                  for (char c : std::string("CFGMSG,0,4,1"))
                      x ^= static_cast<uint8_t>(c);
                  char hex[3];
                  std::snprintf(hex, sizeof(hex), "%02X", x);
                  return std::string(hex);
              }()) + "\r\n");
        // A known sentence: $PUBX,00*33 (u-blox position poll)
        CHECK(build("$PUBX,00", true, false) == "$PUBX,00*33");
        // '!' is skipped like '$'; without either, all characters count
        CHECK(build("!A", true, false) == "!A*41");
        CHECK(build("A", true, false) == "A*41");
        // Each option on its own
        CHECK(build("$CFGSAVE", false, true) == "$CFGSAVE\r\n");
        CHECK(build("$CFGSAVE", false, false) == "$CFGSAVE");
        CHECK(build("$CFGSAVE", true, false).size() == std::strlen("$CFGSAVE") + 3);

        bool ok = true;
        const char *error = nullptr;
        build("", true, true, &ok, &error);
        CHECK(!ok && error != nullptr);
        build("$X*12", true, true, &ok, &error); // a checksum typed in with the option on
        CHECK(!ok && error != nullptr && std::strchr(error, '*') != nullptr);
        CHECK(build("$X*12", false, true, &ok) == "$X*12\r\n" && ok); // and with it off
        build("$A\tB", true, true, &ok, &error);
        CHECK(!ok);
        build("$\xC3\xA4", true, true, &ok, &error); // UTF-8
        CHECK(!ok);
        const std::string longest(GNSS_CONSOLE_COMMAND_MAX, 'A');
        CHECK(build(longest.c_str(), true, true, &ok).size() == GNSS_CONSOLE_COMMAND_MAX + 5 && ok);
        const std::string too_long(GNSS_CONSOLE_COMMAND_MAX + 1, 'A');
        build(too_long.c_str(), false, false, &ok, &error);
        CHECK(!ok);
        // A too small output buffer is refused, not overrun
        uint8_t small[10];
        size_t length = 0;
        CHECK(!gnss_console_build_text("$A", true, true, small, sizeof(small), &length, &error));
        CHECK(!gnss_console_build_text(nullptr, true, true, small, sizeof(small), &length, nullptr));
    }

    void test_parse_ubx()
    {
        uint8_t cls = 0;
        uint8_t id = 0;
        uint8_t payload[GNSS_CONSOLE_UBX_PAYLOAD_MAX];
        uint16_t length = 0;
        const char *error = nullptr;

        CHECK(gnss_console_parse_ubx("0A 04", &cls, &id, payload, &length, &error));
        CHECK_EQ(cls, 0x0A);
        CHECK_EQ(id, 0x04);
        CHECK_EQ(length, 0);
        CHECK(gnss_console_parse_ubx("0x06,0x8b:00-00 00 00 01 00 05 40", &cls, &id, payload, &length, &error));
        CHECK_EQ(cls, 0x06);
        CHECK_EQ(id, 0x8B);
        CHECK_EQ(length, 8);
        CHECK_EQ(payload[7], 0x40);
        CHECK(gnss_console_parse_ubx("068A000100000100054000", &cls, &id, payload, &length, &error)); // without separators
        CHECK_EQ(length, 9);

        // A whole frame: checked and taken apart
        const bytes_t frame = ubx_frame(0x06, 0x8A, {0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x05, 0x40, 0x00});
        std::string hex;
        for (uint8_t b : frame)
        {
            char two[4];
            std::snprintf(two, sizeof(two), "%02X ", b);
            hex += two;
        }
        CHECK(gnss_console_parse_ubx(hex.c_str(), &cls, &id, payload, &length, &error));
        CHECK_EQ(cls, 0x06);
        CHECK_EQ(id, 0x8A);
        CHECK_EQ(length, 9);
        std::string wrong = hex;
        wrong[wrong.size() - 2] = wrong[wrong.size() - 2] == '0' ? '1' : '0'; // last checksum digit
        CHECK(!gnss_console_parse_ubx(wrong.c_str(), &cls, &id, payload, &length, &error) && error != nullptr);
        CHECK(!gnss_console_parse_ubx("B5 62 0A 04 01 00 00 00", &cls, &id, payload, &length, &error)); // length 1, none
        CHECK(!gnss_console_parse_ubx("B5 62 0A", &cls, &id, payload, &length, &error));

        // Mistakes
        CHECK(!gnss_console_parse_ubx("", &cls, &id, payload, &length, &error));
        CHECK(!gnss_console_parse_ubx("0A", &cls, &id, payload, &length, &error));
        CHECK(!gnss_console_parse_ubx("0A 4", &cls, &id, payload, &length, &error)); // one digit
        CHECK(!gnss_console_parse_ubx("0A 04 GG", &cls, &id, payload, &length, &error));
        CHECK(!gnss_console_parse_ubx("0x", &cls, &id, payload, &length, &error));
        std::string most = "06 8A";
        for (size_t i = 0; i < GNSS_CONSOLE_UBX_PAYLOAD_MAX; ++i)
            most += " 00";
        CHECK(gnss_console_parse_ubx(most.c_str(), &cls, &id, payload, &length, &error));
        CHECK_EQ(length, GNSS_CONSOLE_UBX_PAYLOAD_MAX);
        CHECK(!gnss_console_parse_ubx((most + " 00").c_str(), &cls, &id, payload, &length, &error));
        CHECK(!gnss_console_parse_ubx((most + " 00 00 00 00 00 00 00 00 00").c_str(), &cls, &id, payload, &length, &error));
        CHECK(!gnss_console_parse_ubx(nullptr, &cls, &id, payload, &length, nullptr));
    }

    void test_describe_ubx()
    {
        CHECK(describe(0x05, 0x01, {0x06, 0x8A}) == "UBX 05-01 ACK for 06-8A");
        CHECK(describe(0x05, 0x00, {0x06, 0x8B}) == "UBX 05-00 NAK for 06-8B");
        CHECK(describe(0x0A, 0x04, {}) == "UBX 0A-04 (0 bytes)");
        CHECK(describe(0x01, 0x07, {0xAB}) == "UBX 01-07 (1 byte): AB");
        CHECK(describe(0x27, 0x03, {0x01, 0x02, 0xFF}) == "UBX 27-03 (3 bytes): 01 02 FF");

        // MON-VER: the strings
        bytes_t version(30 + 10 + 30 + 30, 0);
        std::memcpy(version.data(), "ROM SPG 5.10 (7b202e)", 21);
        std::memcpy(version.data() + 30, "000A0000", 8);
        std::memcpy(version.data() + 40, "FWVER=SPG 5.10", 14);
        std::memcpy(version.data() + 70, "PROTVER=34.10", 13);
        CHECK(describe(0x0A, 0x04, version) == "UBX 0A-04 MON-VER: ROM SPG 5.10 (7b202e) 000A0000 FWVER=SPG 5.10 PROTVER=34.10");
        // A full string without its zero and an unprintable one
        bytes_t odd(40, 'A');
        odd[30] = 0x01;
        CHECK(describe(0x0A, 0x04, odd) == "UBX 0A-04 MON-VER: " + std::string(30, 'A'));

        // A long payload is cut with "..." within the buffer, for every buffer size
        bytes_t big(512);
        for (size_t i = 0; i < big.size(); ++i)
            big[i] = static_cast<uint8_t>(i);
        const std::string cut = describe(0x02, 0x15, big);
        CHECK(cut.size() < GNSS_CONSOLE_TEXT_SIZE);
        CHECK(cut.size() > 4 && cut.compare(cut.size() - 4, 4, " ...") == 0);
        for (size_t size = 1; size < 40; ++size)
        {
            describe(0x02, 0x15, big, size);
            describe(0x0A, 0x04, version, size);
            describe(0x05, 0x01, {0x06, 0x8A}, size);
        }
        gnss_console_describe_ubx(0x01, 0x02, nullptr, 5, nullptr, 0); // no buffer: nothing happens
        char text[32];
        gnss_console_describe_ubx(0x01, 0x02, nullptr, 5, text, sizeof(text));
        CHECK(std::string(text) == "UBX 01-02 (0 bytes)");
    }

    void test_periodic()
    {
        CHECK(gnss_console_line_is_periodic("$GNRMC,123519,A,4807.038,N"));
        CHECK(gnss_console_line_is_periodic("$GPGSV,3,1,11"));
        CHECK(gnss_console_line_is_periodic("$GAGGA,"));
        CHECK(!gnss_console_line_is_periodic("$GNTXT,01,01,02,ANTSTATUS=OK"));
        CHECK(!gnss_console_line_is_periodic("$command,CFGMSG,response: OK*4F"));
        CHECK(!gnss_console_line_is_periodic("$PUBX,00,"));
        CHECK(!gnss_console_line_is_periodic("$GNRMC"));
        CHECK(!gnss_console_line_is_periodic("GNRMC,"));
        CHECK(!gnss_console_line_is_periodic(""));
        CHECK(!gnss_console_line_is_periodic(nullptr));
        CHECK(gnss_console_ubx_is_periodic(0x01, 0x07));
        CHECK(gnss_console_ubx_is_periodic(0x0D, 0x01));
        CHECK(!gnss_console_ubx_is_periodic(0x0D, 0x03));
        CHECK(!gnss_console_ubx_is_periodic(0x05, 0x01));
    }

    void test_ring()
    {
        static gnss_console_ring_t ring{};
        gnss_console_entry_t entry{};
        CHECK(!gnss_console_ring_next(ring, 0, &entry));
        gnss_console_ring_add(ring, GNSS_CONSOLE_SENT, false, "first");
        CHECK(gnss_console_ring_next(ring, 0, &entry) && entry.seq == 1 && std::string(entry.text) == "first");
        CHECK(!gnss_console_ring_next(ring, 1, &entry));
        CHECK(!gnss_console_ring_next(ring, 5, &entry)); // a number from the future
        // Overwritten: the oldest kept entry comes next
        for (int i = 2; i <= 100; ++i)
            gnss_console_ring_add(ring, GNSS_CONSOLE_TEXT, i % 2 == 0, std::to_string(i).c_str());
        CHECK(gnss_console_ring_next(ring, 0, &entry));
        CHECK_EQ(entry.seq, 100 - GNSS_CONSOLE_ENTRIES + 1);
        CHECK(std::string(entry.text) == std::to_string(100 - GNSS_CONSOLE_ENTRIES + 1));
        CHECK(gnss_console_ring_next(ring, 99, &entry) && entry.seq == 100 && entry.periodic);
        // A long text is cut and terminated; nullptr is an empty text
        const std::string long_text(500, 'L');
        gnss_console_ring_add(ring, GNSS_CONSOLE_TEXT, false, long_text.c_str());
        CHECK(gnss_console_ring_next(ring, 100, &entry) && std::strlen(entry.text) == GNSS_CONSOLE_TEXT_SIZE - 1);
        gnss_console_ring_add(ring, GNSS_CONSOLE_TEXT, false, nullptr);
        CHECK(gnss_console_ring_next(ring, 101, &entry) && entry.text[0] == '\0');
        CHECK(!gnss_console_ring_next(ring, 0, nullptr));
    }

    void test_reader_recording()
    {
        start();
        // Closed (never opened): nothing is kept, the NMEA parser works as before
        CHECK_EQ(gnss_uart_console_last_seq(), 0);
        feed(std::string("$command,CFGMSG,response: OK*4F\r\n"));
        CHECK(entries_after(0).empty());

        CHECK(gnss_uart_console_open());
        host_gnss_uart_console_capture(true);
        gnss_uart_console_add(GNSS_CONSOLE_SENT, false, "$CFGMSG,0,4,1*XX <CR><LF>");
        feed(std::string("$command,CFGMSG,0,4,1,response: OK*4F\r\n"));
        feed(std::string("$GNRMC,120000.00,A,4807.038,N,01131.000,E,0.0,0.0,281026,,,A*00\r\n"));
        feed(std::string("#VERSIONA,97,GPS,FINE\n")); // a line without '$' (Unicore ASCII log)
        feed(std::string("\r\n\r\n"));                // empty lines are not kept
        feed(ubx_frame(0x05, 0x01, {0x06, 0x8A}));    // ACK
        feed(ubx_frame(0x01, 0x07, bytes_t(92, 0)));  // NAV-PVT: periodic
        feed(std::string("half a li"));                // cut by a UBX frame: dropped
        feed(ubx_frame(0x0D, 0x01, bytes_t(16, 0)));
        feed(std::string("ne\r\n"));                   // "ne" is a line of its own

        const auto list = entries_after(0);
        CHECK_EQ(list.size(), 8u);
        if (list.size() == 8)
        {
            CHECK(list[0].kind == GNSS_CONSOLE_SENT);
            CHECK(std::string(list[1].text) == "$command,CFGMSG,0,4,1,response: OK*4F" && list[1].kind == GNSS_CONSOLE_TEXT && !list[1].periodic);
            CHECK(list[2].periodic && std::strncmp(list[2].text, "$GNRMC,", 7) == 0);
            CHECK(std::string(list[3].text) == "#VERSIONA,97,GPS,FINE");
            CHECK(std::string(list[4].text) == "UBX 05-01 ACK for 06-8A" && list[4].kind == GNSS_CONSOLE_UBX && !list[4].periodic);
            CHECK(list[5].periodic && list[5].kind == GNSS_CONSOLE_UBX);
            CHECK(list[6].periodic && std::strncmp(list[6].text, "UBX 0D-01", 9) == 0);
            CHECK(std::string(list[7].text) == "ne");
            for (size_t i = 1; i < list.size(); ++i)
                CHECK_EQ(list[i].seq, list[i - 1].seq + 1);
        }
        CHECK_EQ(gnss_uart_console_last_seq(), 8);

        // A line longer than an entry is cut, the next one is whole again
        feed(std::string(400, 'Z') + "\r\nshort\r\n");
        const auto more = entries_after(8);
        CHECK_EQ(more.size(), 2u);
        if (more.size() == 2)
        {
            CHECK(std::strlen(more[0].text) == GNSS_CONSOLE_TEXT_SIZE - 1);
            CHECK(std::string(more[1].text) == "short");
        }

        // Recording off again: no entries, the parsers go on
        host_gnss_uart_console_capture(false);
        feed(std::string("$GNTXT,01,01,02,not kept*00\r\n"));
        CHECK(entries_after(10).empty());
    }

    void test_ubx_send_raw()
    {
        start();
        s_written.clear();
        const uint8_t payload[] = {0x00, 0x01, 0x00, 0x00};
        CHECK(gnss_ubx_send_raw(0x06, 0x8B, payload, sizeof(payload), 100));
        CHECK(s_written == ubx_frame(0x06, 0x8B, {0x00, 0x01, 0x00, 0x00}));
        s_written.clear();
        CHECK(gnss_ubx_send_raw(0x0A, 0x04, nullptr, 0, 100));
        CHECK(s_written == ubx_frame(0x0A, 0x04, {}));
        CHECK(!gnss_ubx_send_raw(0x0A, 0x04, nullptr, 3, 100)); // a length without payload
        uint8_t big[GNSS_CONSOLE_UBX_PAYLOAD_MAX + 1] = {};
        CHECK(gnss_ubx_send_raw(0x06, 0x8A, big, GNSS_CONSOLE_UBX_PAYLOAD_MAX, 100)); // the largest the console makes
        s_written.clear();
        CHECK(!gnss_ubx_send_raw(0x06, 0x8A, big, 400, 100)); // longer than the send buffer: refused
        CHECK(s_written.empty());
        // The UART not running: refused
        host_gnss_uart_reset(false);
        CHECK(!gnss_ubx_send_raw(0x0A, 0x04, nullptr, 0, 100));
        CHECK(!gnss_uart_write(payload, sizeof(payload)));
    }
}

#ifndef HOST_FUZZ
int main()
{
    test_build_text();
    test_parse_ubx();
    test_describe_ubx();
    test_periodic();
    test_ring();
    test_reader_recording();
    test_ubx_send_raw();
    std::printf("gnss_console_test: %d checks, %d failed\n", host_checks, host_failures);
    return host_failures == 0 ? 0 : 1;
}
#else
// Fuzzing: the input is a typed command (text and UBX) and, after a zero byte, a byte stream from the receiver that
// the reader records; the outputs must stay within their buffers and keep their rules
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool opened = false;
    if (!opened)
    {
        start();
        opened = gnss_uart_console_open();
        host_gnss_uart_console_capture(true);
    }
    const uint8_t *zero = static_cast<const uint8_t *>(std::memchr(data, 0, size));
    const size_t command_length = zero != nullptr ? static_cast<size_t>(zero - data) : size;
    const std::string command(reinterpret_cast<const char *>(data), command_length);

    for (int flags = 0; flags < 4; ++flags)
    {
        uint8_t out[GNSS_CONSOLE_COMMAND_MAX + 5];
        size_t length = 0;
        const char *error = nullptr;
        if (gnss_console_build_text(command.c_str(), flags & 1, flags & 2, out, sizeof(out), &length, &error))
        {
            if (length > sizeof(out) || length < command.size())
                __builtin_trap();
            for (size_t i = 0; i < length; ++i)
                if ((out[i] < 32 || out[i] > 126) && out[i] != '\r' && out[i] != '\n')
                    __builtin_trap();
        }
        else if (error == nullptr)
            __builtin_trap();
    }

    uint8_t cls = 0;
    uint8_t id = 0;
    uint8_t payload[GNSS_CONSOLE_UBX_PAYLOAD_MAX];
    uint16_t length = 0;
    const char *error = nullptr;
    if (gnss_console_parse_ubx(command.c_str(), &cls, &id, payload, &length, &error))
    {
        if (length > GNSS_CONSOLE_UBX_PAYLOAD_MAX)
            __builtin_trap();
        char text[GNSS_CONSOLE_TEXT_SIZE];
        gnss_console_describe_ubx(cls, id, payload, length, text, sizeof(text));
        if (strnlen(text, sizeof(text)) >= sizeof(text))
            __builtin_trap();
    }
    else if (error == nullptr)
        __builtin_trap();

    // The stream after the zero byte, and the whole input as a UBX payload
    if (zero != nullptr)
        host_gnss_uart_feed(zero + 1, size - command_length - 1, 1);
    char text[GNSS_CONSOLE_TEXT_SIZE];
    const uint8_t frame_cls = size > 0 ? data[0] : 0;
    const uint8_t frame_id = size > 1 ? data[1] : 0;
    gnss_console_describe_ubx(frame_cls, frame_id, data, static_cast<uint16_t>(std::min<size_t>(size, 0xFFFF)), text,
                              sizeof(text));
    if (strnlen(text, sizeof(text)) >= sizeof(text))
        __builtin_trap();
    gnss_console_entry_t entry{};
    uint32_t after = 0;
    while (gnss_uart_console_next(after, &entry))
    {
        if (entry.seq <= after || strnlen(entry.text, sizeof(entry.text)) >= sizeof(entry.text))
            __builtin_trap();
        after = entry.seq;
    }
    return 0;
}
#endif
