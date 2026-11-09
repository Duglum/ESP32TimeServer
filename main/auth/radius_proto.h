// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// RADIUS packets of the client (pure, no ESP-IDF; MD5 and HMAC-MD5 through PSA): the Access-Request of a PAP login
// (RFC 2865) with a Message-Authenticator (RFC 3579 3.2) as its first attribute, and the checks of an answer.
//
// The checks of an answer (radius_parse_answer), in this order: the length (20 to 4096 bytes, equal to the length
// field; the datagram may be longer, the rest is ignored as RFC 2865 3 says), the code (Access-Accept, -Reject or
// -Challenge), the identifier of the request, the Response Authenticator (MD5 of the packet with the Request
// Authenticator and the shared secret), every attribute within the packet (length at least 2), a Message-Authenticator
// (at most one, 16 bytes; required unless require_message_authenticator is off: BlastRADIUS, CVE-2024-3596) and its
// HMAC-MD5. Anything that fails is dropped as if it had not arrived (the client keeps waiting for a valid answer).
//
// The privilege level of an Access-Accept (radius_privilege): from Cisco-AVPair "shell:priv-lvl=N" (also "priv-lvl=N"
// and the optional form with '*'; N 0 to 15; several: the lowest), without it Service-Type Administrative-User = 15,
// anything else = 1 (what a Cisco switch gives a login without an explicit level).
#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t RADIUS_HEADER = 20;
constexpr size_t RADIUS_PACKET_MAX = 4096;
constexpr size_t RADIUS_AUTHENTICATOR = 16;
constexpr size_t RADIUS_SECRET_MAX = 64;
constexpr size_t RADIUS_USER_MAX = 64;       // bytes of a user name (RFC 2865 allows 253; the page: 64)
constexpr size_t RADIUS_PASSWORD_MAX = 128;  // bytes of a PAP password (RFC 2865 5.2)
constexpr size_t RADIUS_TEXT_MAX = 64;       // NAS-Identifier, NAS-Port-Id, Calling-Station-Id
constexpr size_t RADIUS_REQUIRED_MAX = 128;  // value of the attribute an Access-Accept must carry
constexpr size_t RADIUS_SUMMARY_SIZE = 320;  // the attributes of an answer as text (the test of the page)

enum radius_code_t : uint8_t
{
    RADIUS_ACCESS_REQUEST = 1,
    RADIUS_ACCESS_ACCEPT = 2,
    RADIUS_ACCESS_REJECT = 3,
    RADIUS_ACCESS_CHALLENGE = 11,
};

// Attribute types used here
enum radius_attribute_t : uint8_t
{
    RADIUS_USER_NAME = 1,
    RADIUS_USER_PASSWORD = 2,
    RADIUS_NAS_IP_ADDRESS = 4,
    RADIUS_NAS_PORT = 5,
    RADIUS_SERVICE_TYPE = 6,
    RADIUS_FILTER_ID = 11,
    RADIUS_REPLY_MESSAGE = 18,
    RADIUS_CLASS = 25,
    RADIUS_VENDOR_SPECIFIC = 26,
    RADIUS_CALLING_STATION_ID = 31,
    RADIUS_NAS_IDENTIFIER = 32,
    RADIUS_NAS_PORT_TYPE = 61,
    RADIUS_MESSAGE_AUTHENTICATOR = 80,
    RADIUS_NAS_PORT_ID = 87,
    RADIUS_NAS_IPV6_ADDRESS = 95,
};

constexpr uint32_t RADIUS_VENDOR_CISCO = 9;
constexpr uint8_t RADIUS_CISCO_AVPAIR = 1;
constexpr uint32_t RADIUS_SERVICE_LOGIN = 1;
constexpr uint32_t RADIUS_SERVICE_ADMINISTRATIVE = 6;
constexpr uint32_t RADIUS_SERVICE_NAS_PROMPT = 7;
constexpr int RADIUS_PORT_TYPE_ASYNC = 0;
constexpr int RADIUS_PORT_TYPE_VIRTUAL = 5;

// The fields of an Access-Request (texts: nullptr or "" = not sent)
struct radius_request_t
{
    const char *user;
    size_t user_length;         // 1 to RADIUS_USER_MAX
    const char *password;
    size_t password_length;     // 0 to RADIUS_PASSWORD_MAX
    const char *nas_identifier; // at most RADIUS_TEXT_MAX bytes each
    const char *nas_port_id;
    const char *calling_station_id;
    bool nas_ip_set;
    uint8_t nas_ip[4];
    bool nas_ipv6_set;
    uint8_t nas_ipv6[16];
    bool nas_port_set;
    uint32_t nas_port;
    int nas_port_type;     // -1 = not sent
    uint32_t service_type; // 0 = not sent
};

// Builds the Access-Request (identifier and the random Request Authenticator from the caller). Returns its length, 0
// if a field is out of range or out_size is too small (RADIUS_PACKET_MAX is always enough).
size_t radius_build_access_request(const radius_request_t &request, uint8_t identifier, const uint8_t authenticator[RADIUS_AUTHENTICATOR],
                                   const uint8_t *secret, size_t secret_length, uint8_t *out, size_t out_size);

// The attribute an Access-Accept must carry in addition (setting "radius.required", e.g. "Cisco-AVPair=shell:roles=x")
struct radius_required_t
{
    uint8_t type;      // 0 = none, RADIUS_CLASS, RADIUS_FILTER_ID or RADIUS_VENDOR_SPECIFIC (a Cisco-AVPair)
    char value[RADIUS_REQUIRED_MAX + 1];
    size_t length;
};
// "" = none; "Cisco-AVPair=<value>", "Class=<value>" or "Filter-Id=<value>" (names case insensitive, value 1 to
// RADIUS_REQUIRED_MAX printable bytes, compared exactly). false: anything else.
bool radius_parse_required(const char *text, radius_required_t &out);

enum class radius_check_t : uint8_t
{
    ok,
    length,                // shorter than 20 bytes, longer than 4096, or the length field does not fit the datagram
    code,                  // not Access-Accept, -Reject or -Challenge
    identifier,            // not the identifier of the request
    authenticator,         // wrong Response Authenticator (wrong shared secret or forged)
    attribute,             // an attribute does not fit in the packet, or a second Message-Authenticator
    no_message_authenticator,
    message_authenticator, // wrong Message-Authenticator
};
const char *radius_check_text(radius_check_t check);

struct radius_answer_t
{
    uint8_t code;
    bool message_authenticator; // present (and valid)
    uint32_t service_type;      // 0 = none
    int avpair_privilege;       // the lowest shell:priv-lvl, -1 = none
    bool required_found;        // the required attribute is in the answer (true when none is required)
    char summary[RADIUS_SUMMARY_SIZE]; // the attributes as text: Service-Type, Cisco-AVPair, Class, Filter-Id, Reply-Message
};

// Checks an answer (see the top of the file). out is filled only for ok.
radius_check_t radius_parse_answer(const uint8_t *packet, size_t length, uint8_t identifier, const uint8_t request_authenticator[RADIUS_AUTHENTICATOR],
                                   const uint8_t *secret, size_t secret_length, bool require_message_authenticator,
                                   const radius_required_t &required, radius_answer_t &out);

// The privilege level of an answer (1 to 15, 0 from "priv-lvl=0")
int radius_privilege(const radius_answer_t &answer);

// A user name or password the device sends: user 1 to RADIUS_USER_MAX bytes without control characters (UTF-8 is
// passed on as it is), password 1 to RADIUS_PASSWORD_MAX bytes without NUL
bool radius_user_valid(const char *user, size_t length);
bool radius_password_valid(const char *password, size_t length);
