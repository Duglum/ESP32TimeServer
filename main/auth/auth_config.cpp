// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//
// Settings of the logins without ESP-IDF (see auth_config.h). Host tested and fuzzed.

#include "auth/auth_config.h"
#include "net/ip_text.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"

// ---- Defaults ("Reset to defaults" returns to these; owner decision 2026-11-09: like a Cisco switch) ----

#define CS_RADIUS_TIMEOUT_S 3
#define CS_RADIUS_RETRIES 2
#define CS_RADIUS_DEAD_TIME_MIN 2
#define CS_RADIUS_MIN_PRIVILEGE 15

namespace
{
    bool get_bool(const cJSON *object, const char *block, const char *field, bool &out, char *err, size_t err_size)
    {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, field);
        if (item == nullptr)
            return true;
        if (!cJSON_IsBool(item))
        {
            snprintf(err, err_size, "%s, field %s: true or false expected", block, field);
            return false;
        }
        out = cJSON_IsTrue(item);
        return true;
    }

    // A whole number of min to max (the range is checked here: the fields are small)
    bool get_int(const cJSON *object, const char *block, const char *field, long min, long max, long &out, bool &present, char *err,
                 size_t err_size)
    {
        present = false;
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, field);
        if (item == nullptr)
            return true;
        const double value = cJSON_IsNumber(item) ? item->valuedouble : 0.5;
        if (!cJSON_IsNumber(item) || !(value >= static_cast<double>(min) && value <= static_cast<double>(max)) ||
            value != static_cast<double>(static_cast<long>(value)))
        {
            snprintf(err, err_size, "%s, field %s: a whole number of %ld to %ld expected", block, field, min, max);
            return false;
        }
        out = static_cast<long>(value);
        present = true;
        return true;
    }

    template <typename T>
    bool get_field(const cJSON *object, const char *block, const char *field, long min, long max, T &out, char *err, size_t err_size)
    {
        long value = 0;
        bool present = false;
        if (!get_int(object, block, field, min, max, value, present, err, err_size))
            return false;
        if (present)
            out = static_cast<T>(value);
        return true;
    }

    template <size_t N>
    bool get_text(const cJSON *object, const char *block, const char *field, char (&out)[N], char *err, size_t err_size)
    {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, field);
        if (item == nullptr)
            return true;
        if (!cJSON_IsString(item) || item->valuestring == nullptr || strlen(item->valuestring) >= N)
        {
            snprintf(err, err_size, "%s, field %s: a text of at most %u characters expected", block, field, static_cast<unsigned>(N - 1));
            return false;
        }
        snprintf(out, N, "%s", item->valuestring);
        return true;
    }

    bool printable(const char *s)
    {
        for (; *s != '\0'; ++s)
            if (static_cast<unsigned char>(*s) < 0x20 || *s == 0x7F)
                return false;
        return true;
    }

    bool ascii_printable(const char *s)
    {
        for (; *s != '\0'; ++s)
            if (static_cast<unsigned char>(*s) < 0x20 || static_cast<unsigned char>(*s) > 0x7E)
                return false;
        return true;
    }
} // namespace

void radius_settings_defaults(radius_settings_t &out)
{
    out = radius_settings_t{};
    for (radius_server_settings_t &s : out.server)
        s.port = AUTH_RADIUS_DEFAULT_PORT;
    out.timeout_s = CS_RADIUS_TIMEOUT_S;
    out.retries = CS_RADIUS_RETRIES;
    out.dead_time_min = CS_RADIUS_DEAD_TIME_MIN;
    out.service_type = RADIUS_SERVICE_LOGIN;
    out.nas_port_type = RADIUS_PORT_TYPE_VIRTUAL;
    out.min_privilege = CS_RADIUS_MIN_PRIVILEGE;
    out.require_message_authenticator = true;
}

void web_login_settings_defaults(web_login_settings_t &out)
{
    out = web_login_settings_t{};
    out.local = true;
    out.local_fallback = true;
}

bool radius_server_used(const radius_server_settings_t &s)
{
    return s.host[0] != '\0';
}

bool radius_any_server(const radius_settings_t &s)
{
    for (const radius_server_settings_t &server : s.server)
        if (radius_server_used(server))
            return true;
    return false;
}

bool radius_host_valid(const char *host)
{
    uint8_t address[16];
    if (ip_parse_ipv4(host, address) || ip_parse_ipv6(host, address))
        return true;
    const size_t length = strnlen(host, AUTH_RADIUS_HOST_SIZE);
    if (length == 0 || length >= AUTH_RADIUS_HOST_SIZE)
        return false;
    size_t label = 0;
    bool all_digits = true;
    char previous = '.';
    for (size_t i = 0; i <= length; ++i)
    {
        const char c = host[i];
        if (c == '.' || c == '\0')
        {
            if (label == 0 || label > 63 || previous == '-')
                return false;
            label = 0;
            previous = '.';
            continue;
        }
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '-')
            return false;
        if (c == '-' && label == 0)
            return false;
        if (!digit)
            all_digits = false;
        ++label;
        previous = c;
    }
    return !all_digits; // "1.2.3" is no name
}

bool radius_settings_validate(const radius_settings_t &s, bool radius_needed, char *err, size_t err_size)
{
    char dummy[8];
    if (err == nullptr || err_size == 0)
    {
        err = dummy;
        err_size = sizeof(dummy);
    }
    err[0] = '\0';
    for (size_t i = 0; i < RADIUS_SERVERS; ++i)
    {
        const radius_server_settings_t &server = s.server[i];
        const unsigned n = static_cast<unsigned>(i + 1);
        if (!radius_server_used(server))
            continue;
        if (!radius_host_valid(server.host))
        {
            snprintf(err, err_size, "RADIUS server %u: an IPv4 or IPv6 address or a DNS name expected", n);
            return false;
        }
        if (server.port == 0)
        {
            snprintf(err, err_size, "RADIUS server %u: UDP port 1 to 65535", n);
            return false;
        }
        if (server.secret[0] == '\0')
        {
            snprintf(err, err_size, "RADIUS server %u: the shared secret is missing", n);
            return false;
        }
        if (!ascii_printable(server.secret))
        {
            snprintf(err, err_size, "RADIUS server %u: shared secret of 1 to %u printable ASCII characters", n,
                     static_cast<unsigned>(RADIUS_SECRET_MAX));
            return false;
        }
    }
    if (radius_needed && !radius_any_server(s))
    {
        snprintf(err, err_size, "RADIUS logins need at least one RADIUS server (Security).");
        return false;
    }
    if (s.timeout_s < RADIUS_TIMEOUT_MIN_S || s.timeout_s > RADIUS_TIMEOUT_MAX_S)
    {
        snprintf(err, err_size, "RADIUS timeout: %u to %u s", static_cast<unsigned>(RADIUS_TIMEOUT_MIN_S), static_cast<unsigned>(RADIUS_TIMEOUT_MAX_S));
        return false;
    }
    if (s.retries > RADIUS_RETRIES_MAX)
    {
        snprintf(err, err_size, "RADIUS retries: 0 to %u", static_cast<unsigned>(RADIUS_RETRIES_MAX));
        return false;
    }
    if (s.dead_time_min > RADIUS_DEAD_TIME_MAX_MIN)
    {
        snprintf(err, err_size, "RADIUS dead time: 0 to %u minutes", static_cast<unsigned>(RADIUS_DEAD_TIME_MAX_MIN));
        return false;
    }
    if (!printable(s.nas_identifier))
    {
        snprintf(err, err_size, "NAS-Identifier: at most %u characters, no control characters", static_cast<unsigned>(RADIUS_TEXT_MAX));
        return false;
    }
    uint8_t address[16];
    if (s.nas_ip[0] != '\0' && !ip_parse_ipv4(s.nas_ip, address))
    {
        snprintf(err, err_size, "NAS-IP-Address: an IPv4 address or empty");
        return false;
    }
    if (s.nas_ipv6[0] != '\0' && !ip_parse_ipv6(s.nas_ipv6, address))
    {
        snprintf(err, err_size, "NAS-IPv6-Address: an IPv6 address or empty");
        return false;
    }
    if (s.service_type != 0 && s.service_type != RADIUS_SERVICE_LOGIN && s.service_type != RADIUS_SERVICE_ADMINISTRATIVE &&
        s.service_type != RADIUS_SERVICE_NAS_PROMPT)
    {
        snprintf(err, err_size, "Service-Type: 0 (not sent), 1, 6 or 7");
        return false;
    }
    if (s.nas_port_type != -1 && s.nas_port_type != RADIUS_PORT_TYPE_ASYNC && s.nas_port_type != RADIUS_PORT_TYPE_VIRTUAL)
    {
        snprintf(err, err_size, "NAS-Port-Type: -1 (not sent), 0 or 5");
        return false;
    }
    if (s.min_privilege < 1 || s.min_privilege > 15)
    {
        snprintf(err, err_size, "Privilege level at least: 1 to 15");
        return false;
    }
    radius_required_t required{};
    if (!radius_parse_required(s.required, required))
    {
        snprintf(err, err_size, "Required attribute: Cisco-AVPair=..., Class=... or Filter-Id=... with a value of 1 to %u characters, or empty",
                 static_cast<unsigned>(RADIUS_REQUIRED_MAX));
        return false;
    }
    return true;
}

bool web_login_settings_validate(const web_login_settings_t &s, char *err, size_t err_size)
{
    if (!s.local && !s.radius)
    {
        snprintf(err, err_size, "At least one login method must stay on.");
        return false;
    }
    return true;
}

void auth_settings_to_json(cJSON *object, const radius_settings_t &r, const web_login_settings_t &w, bool with_secrets)
{
    cJSON *radius = cJSON_AddObjectToObject(object, "radius");
    cJSON *servers = cJSON_AddArrayToObject(radius, "servers");
    for (const radius_server_settings_t &s : r.server)
    {
        cJSON *server = cJSON_CreateObject();
        cJSON_AddStringToObject(server, "host", s.host);
        cJSON_AddNumberToObject(server, "port", s.port);
        if (with_secrets)
            cJSON_AddStringToObject(server, "secret", s.secret);
        else
            cJSON_AddBoolToObject(server, "secret_set", s.secret[0] != '\0');
        cJSON_AddItemToArray(servers, server);
    }
    cJSON_AddNumberToObject(radius, "timeout_s", r.timeout_s);
    cJSON_AddNumberToObject(radius, "retries", r.retries);
    cJSON_AddNumberToObject(radius, "dead_time_min", r.dead_time_min);
    cJSON_AddStringToObject(radius, "nas_identifier", r.nas_identifier);
    cJSON_AddStringToObject(radius, "nas_ip", r.nas_ip);
    cJSON_AddStringToObject(radius, "nas_ipv6", r.nas_ipv6);
    cJSON_AddNumberToObject(radius, "service_type", r.service_type);
    cJSON_AddNumberToObject(radius, "nas_port_type", r.nas_port_type);
    cJSON_AddNumberToObject(radius, "min_privilege", r.min_privilege);
    cJSON_AddStringToObject(radius, "required", r.required);
    cJSON_AddBoolToObject(radius, "require_message_authenticator", r.require_message_authenticator);

    cJSON *web = cJSON_AddObjectToObject(object, "web_login");
    cJSON_AddBoolToObject(web, "local", w.local);
    cJSON_AddBoolToObject(web, "radius", w.radius);
    cJSON_AddBoolToObject(web, "local_fallback", w.local_fallback);
    cJSON_AddBoolToObject(web, "totp_local", w.totp_local);
    cJSON_AddBoolToObject(web, "totp_radius", w.totp_radius);
    cJSON_AddBoolToObject(web, "totp_enroll", w.totp_enroll);
}

bool auth_settings_from_json(const cJSON *root, radius_settings_t &r, web_login_settings_t &w, bool api, char *err, size_t err_size)
{
    const cJSON *radius = cJSON_GetObjectItemCaseSensitive(root, "radius");
    if (radius != nullptr)
    {
        if (!cJSON_IsObject(radius))
        {
            snprintf(err, err_size, "RADIUS: the settings are not a JSON object");
            return false;
        }
        const cJSON *servers = cJSON_GetObjectItemCaseSensitive(radius, "servers");
        if (servers != nullptr)
        {
            if (!cJSON_IsArray(servers) || cJSON_GetArraySize(servers) > static_cast<int>(RADIUS_SERVERS))
            {
                snprintf(err, err_size, "RADIUS servers: a list of at most %u servers expected", static_cast<unsigned>(RADIUS_SERVERS));
                return false;
            }
            size_t i = 0;
            const cJSON *item = nullptr;
            cJSON_ArrayForEach(item, servers)
            {
                radius_server_settings_t &s = r.server[i++];
                if (!cJSON_IsObject(item))
                {
                    snprintf(err, err_size, "RADIUS server %u: an object with host, port and secret expected", static_cast<unsigned>(i));
                    return false;
                }
                char secret[AUTH_RADIUS_SECRET_SIZE] = "";
                const cJSON *secret_item = cJSON_GetObjectItemCaseSensitive(item, "secret");
                if (!get_text(item, "RADIUS server", "host", s.host, err, err_size) ||
                    !get_field(item, "RADIUS server", "port", 1, 65535, s.port, err, err_size) ||
                    !get_text(item, "RADIUS server", "secret", secret, err, err_size))
                    return false;
                // The API never gets the secret: there "" (or none) keeps it; the stored form sets it as it is
                if (secret_item != nullptr && (!api || secret[0] != '\0'))
                    snprintf(s.secret, sizeof(s.secret), "%s", secret);
                memset(secret, 0, sizeof(secret));
                if (!radius_server_used(s))
                    memset(s.secret, 0, sizeof(s.secret));
            }
        }
        if (!get_field(radius, "RADIUS", "timeout_s", 0, 255, r.timeout_s, err, err_size) ||
            !get_field(radius, "RADIUS", "retries", 0, 255, r.retries, err, err_size) ||
            !get_field(radius, "RADIUS", "dead_time_min", 0, 255, r.dead_time_min, err, err_size) ||
            !get_text(radius, "RADIUS", "nas_identifier", r.nas_identifier, err, err_size) ||
            !get_text(radius, "RADIUS", "nas_ip", r.nas_ip, err, err_size) ||
            !get_text(radius, "RADIUS", "nas_ipv6", r.nas_ipv6, err, err_size) ||
            !get_field(radius, "RADIUS", "service_type", 0, 255, r.service_type, err, err_size) ||
            !get_field(radius, "RADIUS", "nas_port_type", -1, 127, r.nas_port_type, err, err_size) ||
            !get_field(radius, "RADIUS", "min_privilege", 0, 255, r.min_privilege, err, err_size) ||
            !get_text(radius, "RADIUS", "required", r.required, err, err_size) ||
            !get_bool(radius, "RADIUS", "require_message_authenticator", r.require_message_authenticator, err, err_size))
            return false;
    }

    const cJSON *web = cJSON_GetObjectItemCaseSensitive(root, "web_login");
    if (web == nullptr)
        return true;
    if (!cJSON_IsObject(web))
    {
        snprintf(err, err_size, "Web login: the settings are not a JSON object");
        return false;
    }
    return get_bool(web, "Web login", "local", w.local, err, err_size) && get_bool(web, "Web login", "radius", w.radius, err, err_size) &&
           get_bool(web, "Web login", "local_fallback", w.local_fallback, err, err_size) &&
           get_bool(web, "Web login", "totp_local", w.totp_local, err, err_size) &&
           get_bool(web, "Web login", "totp_radius", w.totp_radius, err, err_size) &&
           get_bool(web, "Web login", "totp_enroll", w.totp_enroll, err, err_size);
}

radius_timing_t radius_timing_of(const radius_settings_t &s)
{
    radius_timing_t t{};
    for (size_t i = 0; i < RADIUS_SERVERS; ++i)
        t.server_used[i] = radius_server_used(s.server[i]);
    t.timeout_ms = s.timeout_s * 1000u;
    t.retries = s.retries;
    t.dead_time_ms = s.dead_time_min * 60000u;
    return t;
}
