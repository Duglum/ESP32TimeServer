// ESP32 Time Server host tests: the login settings (main/auth/auth_config.cpp, taken from ESP32-ConsoleServer):
// defaults, the JSON form of the page (API) and of the stored settings / configuration file, the validation, the
// shared secrets. The cases follow test_auth of ESP32-ConsoleServer's settings_test (without the SSH fields).
#define TEST_NAME "auth_config_test"
//
// Fuzz target: the input as the JSON of a request and of a configuration file. An accepted and valid set must
// survive the stored form (to JSON with the secrets and back) unchanged, and the API form never carries a secret.

#include "check.h"

#include "auth/auth_config.h"
#include "cJSON.h"

#include <string>

namespace
{
    struct login_t
    {
        radius_settings_t r;
        web_login_settings_t w;
    };

    login_t defaults()
    {
        login_t d{};
        radius_settings_defaults(d.r);
        web_login_settings_defaults(d.w);
        return d;
    }

    // Field by field (the structs have padding)
    bool same(const login_t &a, const login_t &b)
    {
        for (size_t i = 0; i < RADIUS_SERVERS; ++i)
            if (strcmp(a.r.server[i].host, b.r.server[i].host) != 0 || a.r.server[i].port != b.r.server[i].port ||
                strcmp(a.r.server[i].secret, b.r.server[i].secret) != 0)
                return false;
        return a.r.timeout_s == b.r.timeout_s && a.r.retries == b.r.retries && a.r.dead_time_min == b.r.dead_time_min &&
               strcmp(a.r.nas_identifier, b.r.nas_identifier) == 0 && strcmp(a.r.nas_ip, b.r.nas_ip) == 0 &&
               strcmp(a.r.nas_ipv6, b.r.nas_ipv6) == 0 && a.r.service_type == b.r.service_type && a.r.nas_port_type == b.r.nas_port_type &&
               a.r.min_privilege == b.r.min_privilege && strcmp(a.r.required, b.r.required) == 0 &&
               a.r.require_message_authenticator == b.r.require_message_authenticator && a.w.local == b.w.local && a.w.radius == b.w.radius &&
               a.w.local_fallback == b.w.local_fallback && a.w.totp_local == b.w.totp_local && a.w.totp_radius == b.w.totp_radius &&
               a.w.totp_enroll == b.w.totp_enroll;
    }

    // The fields of a request onto base (the page: api = true)
    bool apply(const char *json, const login_t &base, login_t &out, char *err, size_t err_size, bool api = true)
    {
        out = base;
        cJSON *root = cJSON_Parse(json);
        if (root == nullptr)
        {
            snprintf(err, err_size, "not JSON");
            return false;
        }
        const bool ok = auth_settings_from_json(root, out.r, out.w, api, err, err_size);
        cJSON_Delete(root);
        return ok;
    }

    // As web_settings_validate() does it: the login methods, then the servers (needed while RADIUS logins are on)
    bool valid(const login_t &s, char *err, size_t err_size)
    {
        return web_login_settings_validate(s.w, err, err_size) && radius_settings_validate(s.r, s.w.radius, err, err_size);
    }

    std::string to_text(const login_t &s, bool with_secrets)
    {
        cJSON *root = cJSON_CreateObject();
        auth_settings_to_json(root, s.r, s.w, with_secrets);
        char *text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        std::string out = text != nullptr ? text : "";
        cJSON_free(text);
        return out;
    }

    void test_defaults()
    {
        const login_t d = defaults();
        CHECK(d.w.local && !d.w.radius && d.w.local_fallback && !d.w.totp_local && !d.w.totp_radius && !d.w.totp_enroll);
        CHECK(!radius_any_server(d.r) && d.r.server[0].port == 1812 && d.r.server[1].port == 1812);
        CHECK(d.r.timeout_s == 3 && d.r.retries == 2 && d.r.dead_time_min == 2);
        CHECK(d.r.service_type == 1 && d.r.nas_port_type == 5 && d.r.min_privilege == 15);
        CHECK(d.r.require_message_authenticator && d.r.required[0] == '\0');
        char err[200];
        CHECK(valid(d, err, sizeof(err)));
        const radius_timing_t timing = radius_timing_of(d.r);
        CHECK(!timing.server_used[0] && timing.timeout_ms == 3000 && timing.retries == 2 && timing.dead_time_ms == 120000);
    }

    void test_fields()
    {
        const login_t d = defaults();
        login_t s;
        char err[200];
        // refused by the JSON checks or by the validation, with this message
        auto invalid = [&](const std::string &json, const char *error) {
            login_t out;
            char message[200] = "";
            if (apply(json.c_str(), d, out, message, sizeof(message)) && valid(out, message, sizeof(message)))
                strcpy(message, "(accepted)");
            if (strcmp(message, error) != 0)
                fprintf(stderr, "%s: error \"%s\", expected \"%s\"\n", json.c_str(), message, error);
            CHECK(strcmp(message, error) == 0);
        };
        const char *one = "{\"radius\":{\"servers\":[{\"host\":\"192.168.8.100\",\"secret\":\"testing123\"}]}";
        // Login methods
        invalid("{\"web_login\":{\"local\":false}}", "At least one login method must stay on.");
        invalid("{\"web_login\":{\"radius\":true}}", "RADIUS logins need at least one RADIUS server (Security).");
        CHECK(apply((std::string(one) + ",\"web_login\":{\"local\":false,\"radius\":true}}").c_str(), d, s, err, sizeof(err)) &&
              valid(s, err, sizeof(err)));
        CHECK(!s.w.local && s.w.radius && strcmp(s.r.server[0].secret, "testing123") == 0 && s.r.server[0].port == 1812);

        const struct
        {
            const char *json;
            const char *error;
        } cases[] = {
            {"{\"radius\":{\"servers\":[{\"host\":\"radius.example.org\"}]}}", "RADIUS server 1: the shared secret is missing"},
            {"{\"radius\":{\"servers\":[{},{\"host\":\"-x.example.org\",\"secret\":\"s\"}]}}", "RADIUS server 2: an IPv4 or IPv6 address or a DNS name expected"},
            {"{\"radius\":{\"servers\":[{\"host\":\"1.2.3\",\"secret\":\"s\"}]}}", "RADIUS server 1: an IPv4 or IPv6 address or a DNS name expected"},
            {"{\"radius\":{\"servers\":[{\"host\":\"a_b\",\"secret\":\"s\"}]}}", "RADIUS server 1: an IPv4 or IPv6 address or a DNS name expected"},
            {"{\"radius\":{\"servers\":[{\"host\":\"x.\",\"secret\":\"s\"}]}}", "RADIUS server 1: an IPv4 or IPv6 address or a DNS name expected"},
            {"{\"radius\":{\"servers\":[{\"host\":\"a\",\"secret\":\"tab\\there\"}]}}", "RADIUS server 1: shared secret of 1 to 64 printable ASCII characters"},
            {"{\"radius\":{\"servers\":[{\"host\":\"a\",\"secret\":\"\\u00e4\"}]}}", "RADIUS server 1: shared secret of 1 to 64 printable ASCII characters"},
            {"{\"radius\":{\"servers\":[{\"host\":\"a\",\"port\":0,\"secret\":\"s\"}]}}", "RADIUS server, field port: a whole number of 1 to 65535 expected"},
            {"{\"radius\":{\"servers\":[{\"host\":\"a\",\"port\":65536,\"secret\":\"s\"}]}}", "RADIUS server, field port: a whole number of 1 to 65535 expected"},
            {"{\"radius\":{\"servers\":[{},{},{}]}}", "RADIUS servers: a list of at most 2 servers expected"},
            {"{\"radius\":{\"servers\":[1]}}", "RADIUS server 1: an object with host, port and secret expected"},
            {"{\"radius\":{\"servers\":{}}}", "RADIUS servers: a list of at most 2 servers expected"},
            {"{\"radius\":[]}", "RADIUS: the settings are not a JSON object"},
            {"{\"web_login\":1}", "Web login: the settings are not a JSON object"},
            {"{\"web_login\":{\"totp_enroll\":1}}", "Web login, field totp_enroll: true or false expected"},
            {"{\"radius\":{\"timeout_s\":0}}", "RADIUS timeout: 1 to 30 s"},
            {"{\"radius\":{\"timeout_s\":31}}", "RADIUS timeout: 1 to 30 s"},
            {"{\"radius\":{\"timeout_s\":2.5}}", "RADIUS, field timeout_s: a whole number of 0 to 255 expected"},
            {"{\"radius\":{\"retries\":6}}", "RADIUS retries: 0 to 5"},
            {"{\"radius\":{\"dead_time_min\":61}}", "RADIUS dead time: 0 to 60 minutes"},
            {"{\"radius\":{\"nas_identifier\":\"a\\nb\"}}", "NAS-Identifier: at most 64 characters, no control characters"},
            {"{\"radius\":{\"nas_ip\":\"2001:db8::1\"}}", "NAS-IP-Address: an IPv4 address or empty"},
            {"{\"radius\":{\"nas_ipv6\":\"10.0.0.1\"}}", "NAS-IPv6-Address: an IPv6 address or empty"},
            {"{\"radius\":{\"service_type\":2}}", "Service-Type: 0 (not sent), 1, 6 or 7"},
            {"{\"radius\":{\"nas_port_type\":1}}", "NAS-Port-Type: -1 (not sent), 0 or 5"},
            {"{\"radius\":{\"nas_port_type\":-2}}", "RADIUS, field nas_port_type: a whole number of -1 to 127 expected"},
            {"{\"radius\":{\"min_privilege\":0}}", "Privilege level at least: 1 to 15"},
            {"{\"radius\":{\"min_privilege\":16}}", "Privilege level at least: 1 to 15"},
            {"{\"radius\":{\"required\":\"Reply-Message=x\"}}", "Required attribute: Cisco-AVPair=..., Class=... or Filter-Id=... with a value of 1 to 128 characters, or empty"},
        };
        for (const auto &c : cases)
            invalid(c.json, c.error);
        const std::string long_id(65, 'n');
        invalid("{\"radius\":{\"nas_identifier\":\"" + long_id + "\"}}", "RADIUS, field nas_identifier: a text of at most 64 characters expected");
        const std::string long_secret(65, 's');
        invalid("{\"radius\":{\"servers\":[{\"host\":\"a\",\"secret\":\"" + long_secret + "\"}]}}",
                "RADIUS server, field secret: a text of at most 64 characters expected");
        CHECK(apply(("{\"radius\":{\"servers\":[{\"host\":\"a\",\"secret\":\"" + long_secret.substr(1) + "\"}]}}").c_str(), d, s, err, sizeof(err)) &&
              valid(s, err, sizeof(err)));
        const std::string longest_host = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
        CHECK(longest_host.size() == 253 && radius_host_valid(longest_host.c_str()));
        CHECK(!radius_host_valid((longest_host + "d").c_str()) && !radius_host_valid((std::string(64, 'a') + ".b").c_str()));
        CHECK(radius_host_valid("radius") && radius_host_valid("10.0.0.1") && radius_host_valid("fd00::1812") && radius_host_valid("1a.example"));
        CHECK(!radius_host_valid("") && !radius_host_valid("a..b") && !radius_host_valid("a-.b") && !radius_host_valid("[fd00::1]"));
        for (const char *ok : {"{\"radius\":{\"service_type\":0,\"nas_port_type\":-1}}", "{\"radius\":{\"service_type\":6,\"nas_port_type\":0}}",
                               "{\"radius\":{\"service_type\":7,\"min_privilege\":1,\"required\":\"Cisco-AVPair=shell:roles=\\\"x\\\"\"}}",
                               "{\"radius\":{\"timeout_s\":30,\"retries\":5,\"dead_time_min\":60}}", "{\"radius\":{\"timeout_s\":1,\"retries\":0,\"dead_time_min\":0}}",
                               "{\"radius\":{\"nas_ip\":\"192.0.2.1\",\"nas_ipv6\":\"2001:db8::1\"}}"})
        {
            const bool accepted = apply(ok, d, s, err, sizeof(err)) && valid(s, err, sizeof(err));
            if (!accepted)
                fprintf(stderr, "%s: %s\n", ok, err);
            CHECK(accepted);
        }
        // Missing fields keep their value
        CHECK(apply("{}", d, s, err, sizeof(err)) && same(s, d));
        CHECK(apply("{\"radius\":{},\"web_login\":{}}", d, s, err, sizeof(err)) && same(s, d));
    }

    // The secrets: the page (API) never gets them, an empty or missing one keeps the stored one, a server without an
    // address loses its secret. The stored form and the configuration file carry them.
    void test_secrets()
    {
        const login_t d = defaults();
        const char *one = "{\"radius\":{\"servers\":[{\"host\":\"192.168.8.100\",\"secret\":\"testing123\"}]}}";
        login_t base;
        login_t s;
        char err[200];
        CHECK(apply(one, d, base, err, sizeof(err)) && valid(base, err, sizeof(err)));
        std::string text = to_text(base, false);
        CHECK(text.find("testing123") == std::string::npos && text.find("\"secret_set\":true") != std::string::npos &&
              text.find("\"secret_set\":false") != std::string::npos);
        text = to_text(base, true);
        CHECK(text.find("\"secret\":\"testing123\"") != std::string::npos && text.find("secret_set") == std::string::npos);
        // The stored form reads back to the same settings
        CHECK(apply(text.c_str(), d, s, err, sizeof(err), false) && same(s, base));
        CHECK(apply("{\"radius\":{\"servers\":[{\"host\":\"192.168.8.100\",\"secret\":\"\"}]}}", base, s, err, sizeof(err)) &&
              strcmp(s.r.server[0].secret, "testing123") == 0);
        CHECK(apply("{\"radius\":{\"servers\":[{\"host\":\"192.168.8.101\"}]}}", base, s, err, sizeof(err)) &&
              strcmp(s.r.server[0].secret, "testing123") == 0 && strcmp(s.r.server[0].host, "192.168.8.101") == 0);
        CHECK(apply("{\"radius\":{\"servers\":[{\"host\":\"192.168.8.100\",\"secret\":\"new one\"}]}}", base, s, err, sizeof(err)) &&
              strcmp(s.r.server[0].secret, "new one") == 0);
        CHECK(apply("{\"radius\":{\"servers\":[{\"host\":\"\"}]}}", base, s, err, sizeof(err)) && s.r.server[0].secret[0] == '\0' &&
              valid(s, err, sizeof(err)));
        // The stored form: "" is an empty secret
        CHECK(apply("{\"radius\":{\"servers\":[{\"host\":\"192.168.8.100\",\"secret\":\"\"}]}}", base, s, err, sizeof(err), false) &&
              s.r.server[0].secret[0] == '\0');
        CHECK(!valid(s, err, sizeof(err)) && strcmp(err, "RADIUS server 1: the shared secret is missing") == 0);
    }
} // namespace

void run_tests()
{
    test_defaults();
    test_fields();
    test_secrets();
}

void fuzz_one(const uint8_t *data, size_t size)
{
    std::string json(reinterpret_cast<const char *>(data), size);
    const login_t d = defaults();
    for (bool api : {true, false})
    {
        login_t s;
        char err[200] = "";
        if (!apply(json.c_str(), d, s, err, sizeof(err), api))
        {
            CHECK(strlen(err) < sizeof(err));
            continue;
        }
        // Texts stay terminated within their fields
        for (const radius_server_settings_t &server : s.r.server)
            CHECK(memchr(server.host, '\0', sizeof(server.host)) != nullptr && memchr(server.secret, '\0', sizeof(server.secret)) != nullptr);
        CHECK(memchr(s.r.nas_identifier, '\0', sizeof(s.r.nas_identifier)) != nullptr && memchr(s.r.required, '\0', sizeof(s.r.required)) != nullptr);
        if (!valid(s, err, sizeof(err)))
            continue;
        // The API form never carries a secret; the stored form reads back unchanged
        const std::string api_text = to_text(s, false);
        CHECK(api_text.find("\"secret\"") == std::string::npos);
        const std::string stored = to_text(s, true);
        login_t back;
        CHECK(apply(stored.c_str(), d, back, err, sizeof(err), false) && same(back, s) && valid(back, err, sizeof(err)));
    }
}
