// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Settings of the logins (pure, no ESP-IDF): the RADIUS servers and the attributes of the requests ("radius"), the
// login methods of the web page and its second factor ("web_login").
//
// JSON (part of the settings, web_settings.h):
//   "radius": {"servers": [{"host": "192.168.8.100", "port": 1812, "secret": "..."}, {...}], "timeout_s": 3,
//              "retries": 2, "dead_time_min": 2, "nas_identifier": "", "nas_ip": "", "nas_ipv6": "",
//              "service_type": 1, "nas_port_type": 5, "min_privilege": 15, "required": "",
//              "require_message_authenticator": true}
//   "web_login": {"local": true, "radius": false, "local_fallback": true, "totp_local": false, "totp_radius": false,
//                 "totp_enroll": false}
// The shared secrets are the only secret fields of the settings: the API (the page) gets "secret_set" instead and an
// empty or missing "secret" keeps the one stored; the stored form and the configuration file carry them (owner
// decision 2026-11-09: so that an import does not lock out a device whose local password is off).
#pragma once

#include <cstddef>
#include <cstdint>

#include "auth/radius_client.h"
#include "auth/radius_proto.h"

struct cJSON;

constexpr size_t AUTH_RADIUS_HOST_SIZE = 254;   // IPv4, IPv6 or a DNS name of at most 253 characters
constexpr size_t AUTH_RADIUS_SECRET_SIZE = RADIUS_SECRET_MAX + 1;
constexpr size_t AUTH_RADIUS_TEXT_SIZE = RADIUS_TEXT_MAX + 1;
constexpr size_t AUTH_RADIUS_REQUIRED_SIZE = 13 + RADIUS_REQUIRED_MAX + 1; // "Cisco-AVPair=" and the value
constexpr uint16_t AUTH_RADIUS_DEFAULT_PORT = 1812;

struct radius_server_settings_t
{
    char host[AUTH_RADIUS_HOST_SIZE]; // "" = not used
    uint16_t port;
    char secret[AUTH_RADIUS_SECRET_SIZE];
};

struct radius_settings_t
{
    radius_server_settings_t server[RADIUS_SERVERS];
    uint8_t timeout_s;      // per try, 1 to 30
    uint8_t retries;        // more tries per server, 0 to 5
    uint8_t dead_time_min;  // 0 to 60; 0 = a server is never skipped
    char nas_identifier[AUTH_RADIUS_TEXT_SIZE]; // "" = the host name
    char nas_ip[16];        // "" = the IPv4 address the request is sent from
    char nas_ipv6[46];      // "" = the IPv6 address the request is sent from
    uint8_t service_type;   // 0 (not sent), 1 Login, 6 Administrative, 7 NAS-Prompt
    int8_t nas_port_type;   // -1 (not sent), 0 Async, 5 Virtual
    uint8_t min_privilege;  // 1 to 15
    char required[AUTH_RADIUS_REQUIRED_SIZE]; // "" or "Cisco-AVPair=...", "Class=...", "Filter-Id=..."
    bool require_message_authenticator;
};

struct web_login_settings_t
{
    bool local;          // the local user "admin" with the password of the page
    bool radius;         // user name and password checked by RADIUS
    bool local_fallback; // with both: the local password only when no RADIUS server answers
    bool totp_local;     // second factor required for the local user
    bool totp_radius;    // second factor required for RADIUS users
    bool totp_enroll;    // a RADIUS user without a token sets one up at the first login
};

// The local user name of the web page (the page's login form has it as a hidden field since step 1)
constexpr char AUTH_WEB_LOCAL_USER[] = "admin";

void radius_settings_defaults(radius_settings_t &out);
void web_login_settings_defaults(web_login_settings_t &out);

// A server is used when it has an address (validation: then also a secret)
bool radius_server_used(const radius_server_settings_t &s);
bool radius_any_server(const radius_settings_t &s);

// Ranges, addresses, texts. radius_needed: a login method uses RADIUS (then at least one server is required).
bool radius_settings_validate(const radius_settings_t &s, bool radius_needed, char *err, size_t err_size);
bool web_login_settings_validate(const web_login_settings_t &s, char *err, size_t err_size);

// Adds "radius" and "web_login" to object; with_secrets: the stored form (else "secret_set")
void auth_settings_to_json(cJSON *object, const radius_settings_t &r, const web_login_settings_t &w, bool with_secrets);
// Reads "radius" and "web_login" of root onto r and w (missing fields keep their value). api: an empty or missing
// secret keeps the one in r; a server whose host is "" loses its secret.
bool auth_settings_from_json(const cJSON *root, radius_settings_t &r, web_login_settings_t &w, bool api, char *err, size_t err_size);

// The timing of a login from the settings
radius_timing_t radius_timing_of(const radius_settings_t &s);

// A DNS name (labels of 1 to 63 letters, digits and '-', not starting or ending with '-', at most 253 characters,
// not all digits) or an IPv4 or IPv6 address
bool radius_host_valid(const char *host);
