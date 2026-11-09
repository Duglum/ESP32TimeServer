// ESP32 Time Server host tests (from ESP32-ConsoleServer): RADIUS packets (main/auth/radius_proto) and the timing
// of a login (main/auth/radius_client)
#define TEST_NAME "radius_test"
//
// Known answers: RFC 2865 7.1 (User-Password, Access-Accept) and the vectors of gen_radius_vectors.py (an independent
// Python reference). Fuzz target: the checks of an answer, with raw datagrams and with attributes in a correctly
// signed packet (so that the fuzzer gets past the Response Authenticator).

#include "check.h"
#include "radius_vectors.h"

#include "auth/radius_client.h"
#include "auth/radius_proto.h"

extern "C"
{
#include "psa/crypto.h"
}

namespace
{
    const uint8_t *u8(const char *s) { return reinterpret_cast<const uint8_t *>(s); }

    // The Response Authenticator of an answer built by a test (no Message-Authenticator)
    void sign(uint8_t *packet, size_t length, const uint8_t ra[16], const char *secret)
    {
        packet[2] = static_cast<uint8_t>(length >> 8);
        packet[3] = static_cast<uint8_t>(length);
        psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
        size_t n = 0;
        psa_hash_setup(&op, PSA_ALG_MD5);
        psa_hash_update(&op, packet, 4);
        psa_hash_update(&op, ra, 16);
        if (length > 20)
            psa_hash_update(&op, packet + 20, length - 20);
        psa_hash_update(&op, u8(secret), strlen(secret));
        uint8_t out[16];
        psa_hash_finish(&op, out, sizeof(out), &n);
        memcpy(packet + 4, out, 16);
    }

    // An Access-Accept with the given attributes, signed with RV_SECRET for RV_RA, identifier 7
    size_t accept_with(uint8_t *packet, const uint8_t *attributes, size_t length)
    {
        packet[0] = RADIUS_ACCESS_ACCEPT;
        packet[1] = 7;
        memcpy(packet + 20, attributes, length);
        sign(packet, 20 + length, RV_RA, RV_SECRET);
        return 20 + length;
    }

    radius_check_t parse(const uint8_t *packet, size_t length, radius_answer_t &answer, bool require_ma = true, const char *required = "")
    {
        radius_required_t r{};
        CHECK(radius_parse_required(required, r));
        return radius_parse_answer(packet, length, 7, RV_RA, u8(RV_SECRET), strlen(RV_SECRET), require_ma, r, answer);
    }

    radius_request_t basic_request(const char *user, const char *password)
    {
        radius_request_t r{};
        r.user = user;
        r.user_length = strlen(user);
        r.password = password;
        r.password_length = strlen(password);
        r.nas_port_type = -1;
        return r;
    }

    void test_rfc2865()
    {
        // RFC 2865 7.1: user nemo, password arctangent, secret xyzzy5461
        static const uint8_t ra[16] = {0x0f, 0x40, 0x3f, 0x94, 0x73, 0x97, 0x80, 0x57, 0xbd, 0x83, 0xd5, 0xcb, 0x98, 0xf4, 0x22, 0x7a};
        static const uint8_t hidden[16] = {0x0d, 0xbe, 0x70, 0x8d, 0x93, 0xd4, 0x13, 0xce, 0x31, 0x96, 0xe4, 0x3f, 0x78, 0x2a, 0x0a, 0xee};
        const radius_request_t r = basic_request("nemo", "arctangent");
        uint8_t packet[RADIUS_PACKET_MAX];
        const size_t length = radius_build_access_request(r, 0, ra, u8("xyzzy5461"), 9, packet, sizeof(packet));
        // Message-Authenticator (18), User-Name (6), User-Password (18)
        CHECK(length == 20 + 18 + 6 + 18);
        CHECK(packet[0] == 1 && packet[1] == 0 && packet[2] == 0 && packet[3] == length);
        CHECK(packet[20] == RADIUS_MESSAGE_AUTHENTICATOR && packet[21] == 18);
        CHECK(packet[38] == RADIUS_USER_NAME && packet[39] == 6 && memcmp(packet + 40, "nemo", 4) == 0);
        CHECK(packet[44] == RADIUS_USER_PASSWORD && packet[45] == 18 && memcmp(packet + 46, hidden, 16) == 0);

        // Its Access-Accept (no Message-Authenticator, as in 1997)
        static const uint8_t accept[] = {0x02, 0x00, 0x00, 0x26, 0x86, 0xfe, 0x22, 0x0e, 0x76, 0x24, 0xba, 0x2a, 0x10,
                                         0x05, 0xf6, 0xbf, 0x9b, 0x55, 0xe0, 0xb2, 0x06, 0x06, 0x00, 0x00, 0x00, 0x01,
                                         0x0f, 0x06, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x06, 0xc0, 0xa8, 0x01, 0x03};
        radius_required_t none{};
        radius_answer_t a{};
        CHECK(radius_parse_answer(accept, sizeof(accept), 0, ra, u8("xyzzy5461"), 9, false, none, a) == radius_check_t::ok);
        CHECK(a.code == RADIUS_ACCESS_ACCEPT && a.service_type == RADIUS_SERVICE_LOGIN && !a.message_authenticator);
        CHECK(radius_privilege(a) == 1);
        CHECK_STR(a.summary, "Service-Type = Login-User");
        CHECK(radius_parse_answer(accept, sizeof(accept), 0, ra, u8("xyzzy5461"), 9, true, none, a) == radius_check_t::no_message_authenticator);
        CHECK(radius_parse_answer(accept, sizeof(accept), 0, ra, u8("xyzzy5462"), 9, false, none, a) == radius_check_t::authenticator);
    }

    void test_requests()
    {
        radius_request_t r = basic_request("jdoe", "correct horse");
        r.nas_identifier = "consoleserver";
        r.nas_ip_set = true;
        const uint8_t ip[4] = {192, 168, 8, 104};
        memcpy(r.nas_ip, ip, 4);
        r.nas_port_set = true;
        r.nas_port = 1;
        r.nas_port_type = RADIUS_PORT_TYPE_VIRTUAL;
        r.service_type = RADIUS_SERVICE_LOGIN;
        r.nas_port_id = "ssh1";
        r.calling_station_id = "192.168.8.100";
        uint8_t packet[RADIUS_PACKET_MAX];
        size_t length = radius_build_access_request(r, 7, RV_RA, u8(RV_SECRET), strlen(RV_SECRET), packet, sizeof(packet));
        CHECK(length == sizeof(RV_REQUEST) && memcmp(packet, RV_REQUEST, sizeof(RV_REQUEST)) == 0);
        // exactly big enough, one byte too small
        CHECK(radius_build_access_request(r, 7, RV_RA, u8(RV_SECRET), strlen(RV_SECRET), packet, sizeof(RV_REQUEST)) == sizeof(RV_REQUEST));
        CHECK(radius_build_access_request(r, 7, RV_RA, u8(RV_SECRET), strlen(RV_SECRET), packet, sizeof(RV_REQUEST) - 1) == 0);

        const char forty[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
        radius_request_t l = basic_request("admin@example.com", forty);
        length = radius_build_access_request(l, 200, RV_RA, u8(RV_SECRET), strlen(RV_SECRET), packet, sizeof(packet));
        CHECK(length == sizeof(RV_REQUEST_LONG) && memcmp(packet, RV_REQUEST_LONG, sizeof(RV_REQUEST_LONG)) == 0);

        // Limits
        char text[300];
        memset(text, 'a', sizeof(text));
        text[sizeof(text) - 1] = '\0';
        radius_request_t bad = basic_request("", "pw");
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) == 0);
        text[RADIUS_USER_MAX + 1] = '\0';
        bad = basic_request(text, "pw");
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) == 0);
        text[RADIUS_USER_MAX] = '\0';
        bad = basic_request(text, "pw");
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) > 0);
        bad = basic_request("a\x01" "b", "pw");
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) == 0);
        memset(text, 'p', sizeof(text));
        text[RADIUS_PASSWORD_MAX] = '\0';
        bad = basic_request("u", text);
        length = radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet));
        CHECK(length == 20 + 18 + 3 + 2 + RADIUS_PASSWORD_MAX);
        text[RADIUS_PASSWORD_MAX] = 'p';
        text[RADIUS_PASSWORD_MAX + 1] = '\0';
        bad = basic_request("u", text);
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) == 0);
        bad = basic_request("u", "");
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) == 20 + 18 + 3 + 18);
        // secret 1 to 64 bytes
        bad = basic_request("u", "pw");
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 0, packet, sizeof(packet)) == 0);
        memset(text, 's', sizeof(text));
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(text), RADIUS_SECRET_MAX, packet, sizeof(packet)) > 0);
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(text), RADIUS_SECRET_MAX + 1, packet, sizeof(packet)) == 0);
        // texts at most 64 bytes
        memset(text, 'n', sizeof(text));
        text[RADIUS_TEXT_MAX] = '\0';
        bad.nas_identifier = text;
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) > 0);
        text[RADIUS_TEXT_MAX] = 'n';
        text[RADIUS_TEXT_MAX + 1] = '\0';
        CHECK(radius_build_access_request(bad, 1, RV_RA, u8(RV_SECRET), 5, packet, sizeof(packet)) == 0);

        CHECK(radius_user_valid("jdoe", 4) && radius_user_valid("dom\\user", 8) && radius_user_valid("j\xc3\xb6rg", 5));
        CHECK(!radius_user_valid("", 0) && !radius_user_valid("a\tb", 3) && !radius_user_valid("a\x7f", 2));
        CHECK(radius_password_valid("x", 1) && !radius_password_valid("", 0) && !radius_password_valid("a\0b", 3));
    }

    void test_answers()
    {
        radius_answer_t a{};
        CHECK(parse(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), a) == radius_check_t::ok);
        CHECK(a.code == RADIUS_ACCESS_ACCEPT && a.message_authenticator && a.service_type == 6 && a.avpair_privilege == 15);
        CHECK(radius_privilege(a) == 15 && a.required_found);
        CHECK_STR(a.summary, "Service-Type = Administrative-User, Cisco-AVPair = \"shell:priv-lvl=15\", Class = \"CS-ADMIN\"");
        CHECK(parse(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), a, true, "Class=CS-ADMIN") == radius_check_t::ok && a.required_found);
        CHECK(parse(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), a, true, "class=CS-ADMIN") == radius_check_t::ok && a.required_found);
        CHECK(parse(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), a, true, "Class=cs-admin") == radius_check_t::ok && !a.required_found);
        CHECK(parse(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), a, true, "Cisco-AVPair=shell:priv-lvl=15") == radius_check_t::ok && a.required_found);
        CHECK(parse(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), a, true, "Filter-Id=CS-ADMIN") == radius_check_t::ok && !a.required_found);

        CHECK(parse(RV_ACCEPT_1, sizeof(RV_ACCEPT_1), a) == radius_check_t::ok && radius_privilege(a) == 1);
        CHECK(parse(RV_ACCEPT_ADMIN_ONLY, sizeof(RV_ACCEPT_ADMIN_ONLY), a) == radius_check_t::ok && radius_privilege(a) == 15);
        CHECK(parse(RV_ACCEPT_PROMPT_ONLY, sizeof(RV_ACCEPT_PROMPT_ONLY), a) == radius_check_t::ok && radius_privilege(a) == 1);
        CHECK(parse(RV_REJECT, sizeof(RV_REJECT), a) == radius_check_t::ok && a.code == RADIUS_ACCESS_REJECT);
        CHECK_STR(a.summary, "Reply-Message = \"Authentication failed\"");

        CHECK(parse(RV_ACCEPT_NO_MA, sizeof(RV_ACCEPT_NO_MA), a) == radius_check_t::no_message_authenticator);
        CHECK(parse(RV_ACCEPT_NO_MA, sizeof(RV_ACCEPT_NO_MA), a, false) == radius_check_t::ok && !a.message_authenticator);
        CHECK(parse(RV_ACCEPT_BAD_MA, sizeof(RV_ACCEPT_BAD_MA), a) == radius_check_t::message_authenticator);
        CHECK(parse(RV_ACCEPT_BAD_MA, sizeof(RV_ACCEPT_BAD_MA), a, false) == radius_check_t::message_authenticator);
        CHECK(parse(RV_ACCEPT_BAD_AUTH, sizeof(RV_ACCEPT_BAD_AUTH), a) == radius_check_t::authenticator);
        CHECK(parse(RV_ACCEPT_WRONG_SECRET, sizeof(RV_ACCEPT_WRONG_SECRET), a) == radius_check_t::authenticator);

        // Identifier, request authenticator, lengths
        radius_required_t none{};
        CHECK(radius_parse_answer(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), 8, RV_RA, u8(RV_SECRET), strlen(RV_SECRET), true, none, a) ==
              radius_check_t::identifier);
        uint8_t other_ra[16];
        memcpy(other_ra, RV_RA, 16);
        other_ra[15] ^= 1;
        CHECK(radius_parse_answer(RV_ACCEPT_15, sizeof(RV_ACCEPT_15), 7, other_ra, u8(RV_SECRET), strlen(RV_SECRET), true, none, a) ==
              radius_check_t::authenticator);
        CHECK(parse(RV_ACCEPT_15, 19, a) == radius_check_t::length);
        CHECK(parse(RV_ACCEPT_15, sizeof(RV_ACCEPT_15) - 1, a) == radius_check_t::length);
        uint8_t longer[sizeof(RV_ACCEPT_15) + 10] = {};
        memcpy(longer, RV_ACCEPT_15, sizeof(RV_ACCEPT_15));
        CHECK(parse(longer, sizeof(longer), a) == radius_check_t::ok); // padding after the packet is ignored
        uint8_t changed[sizeof(RV_ACCEPT_15)];
        memcpy(changed, RV_ACCEPT_15, sizeof(changed));
        changed[0] = RADIUS_ACCESS_REQUEST;
        CHECK(parse(changed, sizeof(changed), a) == radius_check_t::code);
        changed[0] = 4; // Accounting-Request
        CHECK(parse(changed, sizeof(changed), a) == radius_check_t::code);
        memcpy(changed, RV_ACCEPT_15, sizeof(changed));
        changed[sizeof(changed) - 1] ^= 1; // one bit of an attribute
        CHECK(parse(changed, sizeof(changed), a) == radius_check_t::authenticator);
    }

    void test_attributes()
    {
        uint8_t p[RADIUS_PACKET_MAX] = {};
        radius_answer_t a{};
        // an attribute of length 1, one past the end, a second Message-Authenticator, a short one
        const uint8_t one[] = {6, 1, 0, 0};
        CHECK(parse(p, accept_with(p, one, sizeof(one)), a, false) == radius_check_t::attribute);
        const uint8_t past[] = {6, 7, 0, 0, 0, 6};
        CHECK(parse(p, accept_with(p, past, sizeof(past)), a, false) == radius_check_t::attribute);
        const uint8_t single[] = {6};
        CHECK(parse(p, accept_with(p, single, sizeof(single)), a, false) == radius_check_t::attribute);
        uint8_t two_ma[36] = {};
        two_ma[0] = 80;
        two_ma[1] = 18;
        two_ma[18] = 80;
        two_ma[19] = 18;
        CHECK(parse(p, accept_with(p, two_ma, sizeof(two_ma)), a, false) == radius_check_t::attribute);
        const uint8_t short_ma[] = {80, 10, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(parse(p, accept_with(p, short_ma, sizeof(short_ma)), a, false) == radius_check_t::attribute);
        // a wrong Service-Type length is ignored; an empty Class too
        const uint8_t odd[] = {6, 5, 0, 0, 6, 25, 2};
        CHECK(parse(p, accept_with(p, odd, sizeof(odd)), a, false) == radius_check_t::ok && a.service_type == 0);

        // Cisco-AVPair forms
        struct
        {
            const char *avpair;
            int level;
        } forms[] = {{"shell:priv-lvl=15", 15}, {"priv-lvl=7", 7}, {"shell:priv-lvl*3", 3}, {"shell:priv-lvl=0", 0},
                     {"shell:priv-lvl=16", -1}, {"shell:priv-lvl=", -1}, {"shell:priv-lvl=1a", -1}, {"shell:priv-lvl=015", -1},
                     {"shell:priv-lvl", -1}, {"shell:roles=admin", -1}, {"", -1}, {"exec:priv-lvl=15", -1}};
        for (const auto &f : forms)
        {
            uint8_t attrs[64];
            const size_t n = strlen(f.avpair);
            attrs[0] = 26;
            attrs[1] = static_cast<uint8_t>(2 + 4 + 2 + n);
            attrs[2] = 0;
            attrs[3] = 0;
            attrs[4] = 0;
            attrs[5] = 9;
            attrs[6] = 1;
            attrs[7] = static_cast<uint8_t>(2 + n);
            memcpy(attrs + 8, f.avpair, n);
            CHECK(parse(p, accept_with(p, attrs, 8 + n), a, false) == radius_check_t::ok);
            CHECK(a.avpair_privilege == f.level);
        }
        // two AV pairs in one attribute and another vendor: the lowest Cisco level
        const uint8_t pairs[] = {26, 2 + 4 + 2 + 17 + 2 + 16, 0, 0, 0, 9, 1, 2 + 17, 's', 'h', 'e', 'l', 'l', ':', 'p', 'r', 'i', 'v', '-', 'l',
                                 'v', 'l', '=', '1', '5', 1, 2 + 16, 's', 'h', 'e', 'l', 'l', ':', 'p', 'r', 'i', 'v', '-', 'l', 'v', 'l', '=', '5',
                                 26, 2 + 4 + 2 + 16, 0, 0, 0x01, 0x37, 1, 2 + 16, 's', 'h', 'e', 'l', 'l', ':', 'p', 'r', 'i', 'v', '-', 'l', 'v', 'l', '=', '1'};
        CHECK(parse(p, accept_with(p, pairs, sizeof(pairs)), a, false) == radius_check_t::ok && a.avpair_privilege == 5);
        // a sub-attribute past the end of the vendor attribute stops the vendor attribute only
        const uint8_t broken[] = {26, 2 + 4 + 2, 0, 0, 0, 9, 1, 40, 6, 6, 0, 0, 0, 6};
        CHECK(parse(p, accept_with(p, broken, sizeof(broken)), a, false) == radius_check_t::ok && a.avpair_privilege == -1 &&
              a.service_type == 6 && radius_privilege(a) == 15);

        // The summary is cut, never longer than its buffer; non-printable bytes as '?'
        uint8_t many[RADIUS_PACKET_MAX - 20];
        size_t at = 0;
        while (at + 255 <= sizeof(many))
        {
            many[at] = 18;
            many[at + 1] = 255;
            memset(many + at + 2, 'r', 253);
            many[at + 2] = 0x01;
            at += 255;
        }
        CHECK(parse(p, accept_with(p, many, at), a, false) == radius_check_t::ok);
        CHECK(strlen(a.summary) < RADIUS_SUMMARY_SIZE && strncmp(a.summary, "Reply-Message = \"?rrr", 21) == 0);
        // the largest packet: 4096 bytes (a last Class attribute fills it), one byte more is refused
        const size_t last = sizeof(many) - at;
        many[at] = RADIUS_CLASS;
        many[at + 1] = static_cast<uint8_t>(last);
        memset(many + at + 2, 'c', last - 2);
        CHECK(accept_with(p, many, sizeof(many)) == RADIUS_PACKET_MAX);
        CHECK(parse(p, RADIUS_PACKET_MAX, a, false) == radius_check_t::ok);
        p[2] = 0x10;
        p[3] = 0x01; // 4097
        CHECK(parse(p, RADIUS_PACKET_MAX, a, false) == radius_check_t::length);
    }

    void test_required()
    {
        radius_required_t r{};
        CHECK(radius_parse_required("", r) && r.type == 0);
        CHECK(radius_parse_required(nullptr, r) && r.type == 0);
        CHECK(radius_parse_required("Cisco-AVPair=shell:roles=\"console\"", r) && r.type == RADIUS_VENDOR_SPECIFIC &&
              r.length == 21 && strcmp(r.value, "shell:roles=\"console\"") == 0);
        CHECK(radius_parse_required("CLASS=a=b", r) && r.type == RADIUS_CLASS && strcmp(r.value, "a=b") == 0);
        CHECK(radius_parse_required("filter-id=x", r) && r.type == RADIUS_FILTER_ID);
        CHECK(!radius_parse_required("Class=", r) && r.type == 0);
        CHECK(!radius_parse_required("Class", r));
        CHECK(!radius_parse_required("Reply-Message=x", r));
        CHECK(!radius_parse_required("Class=a\tb", r));
        CHECK(!radius_parse_required(" Class=x", r));
        char text[200] = "Class=";
        memset(text + 6, 'v', RADIUS_REQUIRED_MAX);
        CHECK(radius_parse_required(text, r) && r.length == RADIUS_REQUIRED_MAX);
        text[6 + RADIUS_REQUIRED_MAX] = 'v';
        CHECK(!radius_parse_required(text, r));
    }

    // ---- radius_client: the timing of a login ----

    radius_timing_t timing(bool first, bool second, uint32_t retries, uint32_t dead_ms)
    {
        radius_timing_t t{};
        t.server_used[0] = first;
        t.server_used[1] = second;
        t.timeout_ms = 3000;
        t.retries = retries;
        t.dead_time_ms = dead_ms;
        return t;
    }

    void test_client_one_server()
    {
        const radius_timing_t t = timing(true, false, 2, 120000);
        radius_servers_t s{};
        radius_exchange_t x{};
        CHECK(radius_exchange_start(x, t, s, 1000) == radius_step_t::send_new && x.server == 0);
        radius_exchange_sent(x, t, s, 1000);
        CHECK(radius_exchange_tick(x, t, s, 3999) == radius_step_t::wait);
        CHECK(radius_exchange_tick(x, t, s, 4000) == radius_step_t::resend);
        radius_exchange_sent(x, t, s, 4000);
        CHECK(radius_exchange_tick(x, t, s, 7000) == radius_step_t::resend);
        radius_exchange_sent(x, t, s, 7000);
        CHECK(s.server[0].requests == 3);
        CHECK(radius_exchange_tick(x, t, s, 9999) == radius_step_t::wait);
        CHECK(radius_exchange_tick(x, t, s, 10000) == radius_step_t::no_answer && x.done);
        CHECK(s.server[0].timeouts == 1 && s.server[0].dead_until_ms == 130000);
        CHECK(radius_exchange_tick(x, t, s, 20000) == radius_step_t::wait); // done: nothing more
        // the next login gives up at once while the server is dead, and asks it again afterwards
        CHECK(radius_exchange_start(x, t, s, 129999) == radius_step_t::no_answer);
        CHECK(radius_exchange_start(x, t, s, 130000) == radius_step_t::send_new);
        radius_exchange_sent(x, t, s, 130000);
        radius_exchange_answered(x, s, RADIUS_ACCESS_REJECT, 130012);
        CHECK(x.done && s.server[0].rejects == 1 && s.server[0].last_rtt_ms == 12 && s.server[0].last_answer_ms == 130012);
        // the answer to a retransmission: the time since the last packet
        CHECK(radius_exchange_start(x, t, s, 200000) == radius_step_t::send_new);
        radius_exchange_sent(x, t, s, 200000);
        CHECK(radius_exchange_tick(x, t, s, 203000) == radius_step_t::resend);
        radius_exchange_sent(x, t, s, 203000);
        radius_exchange_answered(x, s, RADIUS_ACCESS_ACCEPT, 203005);
        CHECK(s.server[0].accepts == 1 && s.server[0].last_rtt_ms == 5);
    }

    void test_client_failover()
    {
        const radius_timing_t t = timing(true, true, 0, 60000);
        radius_servers_t s{};
        radius_exchange_t x{};
        CHECK(radius_exchange_start(x, t, s, 0) == radius_step_t::send_new && x.server == 0);
        radius_exchange_sent(x, t, s, 0);
        CHECK(radius_exchange_tick(x, t, s, 3000) == radius_step_t::send_new && x.server == 1 && x.tries == 0);
        CHECK(radius_server_dead(s.server[0], 3000) && !radius_server_dead(s.server[1], 3000));
        radius_exchange_sent(x, t, s, 3000);
        radius_exchange_answered(x, s, RADIUS_ACCESS_ACCEPT, 3040);
        CHECK(s.server[1].accepts == 1 && s.server[1].last_rtt_ms == 40 && s.server[0].timeouts == 1);
        // next login: straight to server 2
        CHECK(radius_exchange_start(x, t, s, 4000) == radius_step_t::send_new && x.server == 1);
        radius_exchange_sent(x, t, s, 4000);
        // server 2 is silent too: both dead, no answer
        CHECK(radius_exchange_tick(x, t, s, 7000) == radius_step_t::no_answer);
        CHECK(radius_server_dead(s.server[1], 7000));
        CHECK(radius_exchange_start(x, t, s, 8000) == radius_step_t::no_answer);
        // server 1 is back first (its dead time ends at 63000)
        CHECK(radius_exchange_start(x, t, s, 63000) == radius_step_t::send_new && x.server == 0);

        // only server 2 has an address
        const radius_timing_t second = timing(false, true, 1, 0);
        radius_servers_t s2{};
        CHECK(radius_exchange_start(x, second, s2, 0) == radius_step_t::send_new && x.server == 1);
        radius_exchange_sent(x, second, s2, 0);
        CHECK(radius_exchange_tick(x, second, s2, 3000) == radius_step_t::resend);
        radius_exchange_sent(x, second, s2, 3000);
        CHECK(radius_exchange_tick(x, second, s2, 6000) == radius_step_t::no_answer);
        // dead time 0: never skipped
        CHECK(!radius_server_dead(s2.server[1], 6000) && s2.server[1].timeouts == 1);
        CHECK(radius_exchange_start(x, second, s2, 6000) == radius_step_t::send_new);

        // no server at all
        CHECK(radius_exchange_start(x, timing(false, false, 2, 0), s2, 0) == radius_step_t::no_answer);
        // a packet that could not be sent: the next server at once
        CHECK(radius_exchange_start(x, t, s, 200000) == radius_step_t::send_new && x.server == 0);
        CHECK(radius_exchange_tick(x, t, s, 200000) == radius_step_t::send_new && x.server == 1);
    }

    void test_client_ids()
    {
        radius_ids_t ids{};
        uint8_t id = 0;
        bool seen[256] = {};
        for (int i = 0; i < 256; ++i)
        {
            CHECK(radius_id_take(ids, id));
            CHECK(!seen[id]);
            seen[id] = true;
        }
        CHECK(!radius_id_take(ids, id));
        radius_id_release(ids, 77);
        CHECK(!radius_id_in_use(ids, 77) && radius_id_in_use(ids, 78));
        CHECK(radius_id_take(ids, id) && id == 77);
        // after a release the next one given is not the one just released
        radius_ids_t r{};
        uint8_t a = 0;
        uint8_t b = 0;
        CHECK(radius_id_take(r, a) && a == 0);
        radius_id_release(r, a);
        CHECK(radius_id_take(r, b) && b == 1);
    }
} // namespace

void run_tests()
{
    CHECK(psa_crypto_init() == PSA_SUCCESS);
    test_rfc2865();
    test_requests();
    test_answers();
    test_attributes();
    test_required();
    test_client_one_server();
    test_client_failover();
    test_client_ids();
}

void fuzz_one(const uint8_t *data, size_t size)
{
    static bool init = psa_crypto_init() == PSA_SUCCESS;
    CHECK(init);
    radius_required_t required{};
    radius_parse_required("Class=CS-ADMIN", required);
    radius_answer_t a{};
    // 1. the datagram as it is
    for (const bool require : {true, false})
    {
        const radius_check_t c = radius_parse_answer(data, size, size > 1 ? data[1] : 0, RV_RA, u8(RV_SECRET), strlen(RV_SECRET), require, required, a);
        if (c == radius_check_t::ok)
            CHECK(strlen(a.summary) < RADIUS_SUMMARY_SIZE);
    }
    // 2. the data as the attributes of a correctly signed answer
    static uint8_t p[RADIUS_PACKET_MAX];
    const size_t n = size > RADIUS_PACKET_MAX - 20 ? RADIUS_PACKET_MAX - 20 : size;
    const size_t length = accept_with(p, data, n);
    const radius_check_t c = parse(p, length, a, false, "Cisco-AVPair=shell:priv-lvl=15");
    if (c == radius_check_t::ok)
    {
        CHECK(strlen(a.summary) < RADIUS_SUMMARY_SIZE);
        const int level = radius_privilege(a);
        CHECK(level >= 0 && level <= 15);
        CHECK(a.avpair_privilege >= -1 && a.avpair_privilege <= 15);
    }
    else
        CHECK(c == radius_check_t::attribute || c == radius_check_t::message_authenticator);
}
