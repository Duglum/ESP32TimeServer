// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Web admin page: HTTPS server (TCP 443 only, IPv4 and IPv6) with session login, CSRF protection and the JSON API
// used by the page. The TLS transport is set up here on top of esp_http_server and esp-tls (instead of
// esp_https_server) so that the source address allow-list is checked before the TLS handshake.

#include "web_server.h"
#include "custom/crash_dump.h"
#include "web_auth.h"
#include "web_net.h"
#include "web_ote.h"
#include "web_settings.h"
#include "web_tls.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <strings.h>

#include "ESP32TimeServerSettings.h"

extern "C"
{
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/ssl_ciphersuites.h"
}

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");
extern const char i18n_js_start[] asm("_binary_i18n_js_start");
extern const char i18n_js_end[] asm("_binary_i18n_js_end");
extern const char app_css_start[] asm("_binary_app_css_start");
extern const char app_css_end[] asm("_binary_app_css_end");
extern const char favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const char favicon_ico_end[] asm("_binary_favicon_ico_end");

namespace
{
    const char *TAG = "web_admin";

    // ---- Server configuration ----
    constexpr uint16_t HTTPS_PORT = 443;
    constexpr size_t MAX_SOCKETS = 3;
    constexpr size_t MAX_BODY = 4096;
    constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 5000;
    constexpr uint16_t IO_TIMEOUT_S = 5;
    constexpr UBaseType_t TASK_PRIORITY = 3;   // below every timing task (PPS 14, GNSS 15, NTP 17/20, recovery 6, MQTT 5)
    constexpr BaseType_t TASK_CORE = 0;        // the hardware NTP task runs on core 1
    constexpr size_t TASK_STACK = 10240;       // TLS handshake, PBKDF2 and JSON run in this task
    constexpr size_t START_TASK_STACK = 8192;  // key generation and certificate
    constexpr int64_t NETWORK_CONFIRM_TIMEOUT_US = 120LL * 1000000LL;
    constexpr char SESSION_COOKIE[] = "__Host-sid";

    // TLS 1.2 (ECDHE-ECDSA with AEAD) and TLS 1.3 only
    const int s_ciphersuites[] = {
        MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
        MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        0};

    httpd_handle_t s_server = nullptr;
    std::atomic<bool> s_running{false}; // the HTTPS server answers (web_admin_running)
    esp_tls_cfg_server_t s_tls_config{};
    web_tls_identity_t s_identity{};

    // ---- Network confirmation (confirm-or-rollback) ----
    struct network_pending_t
    {
        bool active;
        bool restart_on_timeout; // the change came with a one-shot boot: a restart returns to the saved settings
        int64_t deadline_us;
        web_settings_t previous; // settings to return to
        web_settings_t applied;
    };
    network_pending_t *s_pending = nullptr; // allocated once, only used in the server task
    esp_timer_handle_t s_confirm_timer = nullptr;

    // ---- TLS transport ----
    struct transport_t
    {
        esp_tls_t *tls;
    };

    void transport_close(void *context)
    {
        transport_t *transport = static_cast<transport_t *>(context);
        if (transport != nullptr)
        {
            esp_tls_server_session_delete(transport->tls);
            free(transport);
        }
    }

    int transport_send(httpd_handle_t server, int sockfd, const char *buf, size_t length, int flags)
    {
        (void)flags;
        transport_t *transport = static_cast<transport_t *>(httpd_sess_get_transport_ctx(server, sockfd));
        if (transport == nullptr)
            return HTTPD_SOCK_ERR_FAIL;
        const ssize_t ret = esp_tls_conn_write(transport->tls, buf, length);
        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE || ret == ESP_TLS_ERR_SSL_WANT_READ)
            return HTTPD_SOCK_ERR_TIMEOUT;
        return ret < 0 ? HTTPD_SOCK_ERR_FAIL : static_cast<int>(ret);
    }

    int transport_recv(httpd_handle_t server, int sockfd, char *buf, size_t length, int flags)
    {
        (void)flags;
        transport_t *transport = static_cast<transport_t *>(httpd_sess_get_transport_ctx(server, sockfd));
        if (transport == nullptr)
            return HTTPD_SOCK_ERR_FAIL;
        const ssize_t ret = esp_tls_conn_read(transport->tls, buf, length);
        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE)
            return HTTPD_SOCK_ERR_TIMEOUT;
        return ret < 0 ? HTTPD_SOCK_ERR_FAIL : static_cast<int>(ret);
    }

    int transport_pending(httpd_handle_t server, int sockfd)
    {
        transport_t *transport = static_cast<transport_t *>(httpd_sess_get_transport_ctx(server, sockfd));
        if (transport == nullptr)
            return 0;
        const ssize_t ret = esp_tls_get_bytes_avail(transport->tls);
        return ret < 0 ? 0 : static_cast<int>(ret);
    }

    // New TCP connection: allow-list first (no TLS work for rejected sources), then the TLS handshake
    esp_err_t transport_open(httpd_handle_t server, int sockfd)
    {
        struct sockaddr_storage peer{};
        socklen_t peer_length = sizeof(peer);
        if (getpeername(sockfd, reinterpret_cast<struct sockaddr *>(&peer), &peer_length) != 0 || !web_auth_source_allowed(peer))
            return ESP_FAIL;

        esp_tls_t *tls = esp_tls_init();
        if (tls == nullptr)
            return ESP_ERR_NO_MEM;
        if (esp_tls_server_session_create(&s_tls_config, sockfd, tls) != 0)
        {
            esp_tls_server_session_delete(tls);
            return ESP_FAIL;
        }
        transport_t *transport = static_cast<transport_t *>(calloc(1, sizeof(transport_t)));
        if (transport == nullptr)
        {
            esp_tls_server_session_delete(tls);
            return ESP_ERR_NO_MEM;
        }
        transport->tls = tls;
        httpd_sess_set_transport_ctx(server, sockfd, transport, transport_close);
        httpd_sess_set_send_override(server, sockfd, transport_send);
        httpd_sess_set_recv_override(server, sockfd, transport_recv);
        httpd_sess_set_pending_override(server, sockfd, transport_pending);
        return ESP_OK;
    }

    // ---- Response helpers ----
    void set_security_headers(httpd_req_t *req, const char *cache_control)
    {
        httpd_resp_set_hdr(req, "Content-Security-Policy",
                           "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; "
                           "form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
        httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
        httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
        httpd_resp_set_hdr(req, "Cross-Origin-Opener-Policy", "same-origin");
        httpd_resp_set_hdr(req, "Cross-Origin-Resource-Policy", "same-origin");
        httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    }

    esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
    {
        char *text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        set_security_headers(req, "no-store");
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json");
        const esp_err_t err = text == nullptr ? httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN)
                                              : httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
        cJSON_free(text);
        return err;
    }

    esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "error", message);
        return send_json(req, status, root);
    }

    esp_err_t send_ok(httpd_req_t *req, const char *message)
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        if (message != nullptr)
            cJSON_AddStringToObject(root, "message", message);
        return send_json(req, "200 OK", root);
    }

    esp_err_t send_static(httpd_req_t *req, const char *type, const char *start, const char *end)
    {
        set_security_headers(req, "no-cache");
        httpd_resp_set_type(req, type);
        // EMBED_TXTFILES appends a NUL that is not part of the file
        return httpd_resp_send(req, start, end - start - 1);
    }

    bool peer_address(httpd_req_t *req, struct sockaddr_storage &address, bool local)
    {
        socklen_t length = sizeof(address);
        address = sockaddr_storage{};
        const int fd = httpd_req_to_sockfd(req);
        return fd >= 0 && (local ? getsockname(fd, reinterpret_cast<struct sockaddr *>(&address), &length)
                                 : getpeername(fd, reinterpret_cast<struct sockaddr *>(&address), &length)) == 0;
    }

    void format_address(const struct sockaddr_storage &address, char *out, size_t size)
    {
        out[0] = '\0';
        if (address.ss_family == AF_INET)
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(&address)->sin_addr, out, size);
        else if (address.ss_family == AF_INET6)
        {
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_addr);
            static constexpr uint8_t v4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
            if (memcmp(bytes, v4_mapped, 12) == 0)
                inet_ntop(AF_INET, bytes + 12, out, size);
            else
                inet_ntop(AF_INET6, bytes, out, size);
        }
    }

    // Reads the session cookie. Returns false if there is none.
    bool session_cookie(httpd_req_t *req, char (&token)[WEB_SESSION_TOKEN_HEX_LEN + 1])
    {
        size_t length = sizeof(token);
        if (httpd_req_get_cookie_val(req, SESSION_COOKIE, token, &length) != ESP_OK)
            return false;
        return strlen(token) == WEB_SESSION_TOKEN_HEX_LEN;
    }

    bool header(httpd_req_t *req, const char *name, char *out, size_t size)
    {
        const size_t length = httpd_req_get_hdr_value_len(req, name);
        if (length == 0 || length >= size)
            return false;
        return httpd_req_get_hdr_value_str(req, name, out, size) == ESP_OK;
    }

    // Cross-site protection of state changing requests: the Origin must be this server (as named in the Host
    // header) and a Sec-Fetch-Site header (if sent) must say same-origin
    bool origin_is_this_server(httpd_req_t *req)
    {
        char host[300];
        char origin[310];
        char expected[310];
        if (!header(req, "Host", host, sizeof(host)) || !header(req, "Origin", origin, sizeof(origin)))
            return false;
        snprintf(expected, sizeof(expected), "https://%s", host);
        if (strcmp(origin, expected) != 0)
            return false;
        char fetch_site[32];
        return !header(req, "Sec-Fetch-Site", fetch_site, sizeof(fetch_site)) || strcmp(fetch_site, "same-origin") == 0;
    }

    // ... and the body must be JSON
    bool same_origin(httpd_req_t *req)
    {
        char content_type[64];
        return origin_is_this_server(req) && header(req, "Content-Type", content_type, sizeof(content_type)) &&
               strncmp(content_type, "application/json", 16) == 0;
    }

    // Reads the JSON body (at most MAX_BODY bytes). Returns nullptr and sends the error response on failure.
    cJSON *read_json(httpd_req_t *req)
    {
        if (req->content_len == 0 || req->content_len > MAX_BODY)
        {
            send_error(req, req->content_len > MAX_BODY ? "413 Payload Too Large" : "400 Bad Request", "Request body missing or too large");
            return nullptr;
        }
        char *body = static_cast<char *>(malloc(req->content_len + 1));
        if (body == nullptr)
        {
            send_error(req, "503 Service Unavailable", "Out of memory");
            return nullptr;
        }
        size_t received = 0;
        int timeouts = 0;
        while (received < req->content_len)
        {
            const int ret = httpd_req_recv(req, body + received, req->content_len - received);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
                continue;
            if (ret <= 0)
            {
                free(body);
                return nullptr; // the connection is closed by httpd
            }
            received += static_cast<size_t>(ret);
        }
        body[received] = '\0';
        cJSON *root = cJSON_ParseWithLength(body, received);
        memset(body, 0, received); // may contain passwords
        free(body);
        if (root == nullptr || !cJSON_IsObject(root))
        {
            cJSON_Delete(root);
            send_error(req, "400 Bad Request", "Invalid JSON");
            return nullptr;
        }
        return root;
    }

    enum class gate_t
    {
        passed,
        rejected, // the response was sent
    };

    // Authentication gate of the API: session cookie; for POST also same origin and the CSRF header
    gate_t check_session(httpd_req_t *req, bool state_changing, char (&token)[WEB_SESSION_TOKEN_HEX_LEN + 1])
    {
        if (!session_cookie(req, token) || !web_auth_session_valid(token, nullptr))
        {
            send_error(req, "401 Unauthorized", "Not logged in");
            return gate_t::rejected;
        }
        if (state_changing)
        {
            char csrf[WEB_CSRF_TOKEN_HEX_LEN + 2];
            if (!same_origin(req) || !header(req, "X-CSRF-Token", csrf, sizeof(csrf)) || !web_auth_csrf_valid(token, csrf))
            {
                send_error(req, "403 Forbidden", "Request rejected (origin or CSRF token)");
                return gate_t::rejected;
            }
        }
        return gate_t::passed;
    }

    // ---- JSON <-> settings ----
    // Names of the LED states in the JSON (order of web_led_state_t)
    const char *const s_led_state_names[WEB_LED_STATE_COUNT] = {"startup", "normal", "sync", "warning", "alert", "critical", "button"};

    void add_ntp_keys(cJSON *array, const web_ntp_key_t *keys, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", keys[i].id);
            cJSON_AddStringToObject(item, "key", keys[i].key);
            cJSON_AddItemToArray(array, item);
        }
    }

    void settings_to_json(cJSON *object, const web_settings_t &s)
    {
        cJSON_AddStringToObject(object, "fqdn", s.fqdn);
        cJSON_AddBoolToObject(object, "ipv4_dhcp", s.ipv4_dhcp);
        cJSON_AddStringToObject(object, "ipv4_address", s.ipv4_address);
        cJSON_AddNumberToObject(object, "ipv4_prefix", s.ipv4_prefix);
        cJSON_AddStringToObject(object, "ipv4_gateway", s.ipv4_gateway);
        cJSON_AddBoolToObject(object, "ipv6_slaac", s.ipv6_slaac);
        cJSON_AddStringToObject(object, "ipv6_address", s.ipv6_address);
        cJSON_AddNumberToObject(object, "ipv6_prefix", s.ipv6_prefix);
        cJSON_AddStringToObject(object, "ipv6_gateway", s.ipv6_gateway);
        cJSON *dns = cJSON_AddArrayToObject(object, "dns");
        for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
            if (s.dns[i][0] != '\0')
                cJSON_AddItemToArray(dns, cJSON_CreateString(s.dns[i]));
        cJSON_AddStringToObject(object, "mac_address", s.mac_address);
        cJSON_AddNumberToObject(object, "ntp_client_rate_limit", s.ntp_client_rate_limit);
        cJSON_AddStringToObject(object, "gnss_protocol", s.gnss_ublox ? "ubx" : "nmea");
        cJSON_AddNumberToObject(object, "gnss_tx_pin", s.gnss_tx_pin);
        cJSON_AddNumberToObject(object, "gnss_rx_pin", s.gnss_rx_pin);
        cJSON_AddNumberToObject(object, "gnss_pps_pin", s.gnss_pps_pin);
        cJSON_AddNumberToObject(object, "gnss_baud", s.gnss_baud);
        cJSON_AddNumberToObject(object, "ptp_loopback_pin", s.ptp_loopback_pin);
        cJSON_AddNumberToObject(object, "pps_holdover", s.pps_holdover_s);
        cJSON_AddNumberToObject(object, "pps_lost_stratum", s.pps_lost_stratum);
        cJSON_AddBoolToObject(object, "pps_lost_serve", s.pps_lost_serve);
        cJSON_AddNumberToObject(object, "pps_lost_serve_min", s.pps_lost_serve_min);
        cJSON_AddNumberToObject(object, "pps_dispersion_ppm", s.pps_dispersion_ppm);
        cJSON_AddNumberToObject(object, "gnss_min_fix_age", s.gnss_min_fix_age_s);
        cJSON_AddNumberToObject(object, "gnss_min_satellites", s.gnss_min_satellites);
        cJSON_AddBoolToObject(object, "lcd_enabled", s.lcd_enabled);
        cJSON_AddStringToObject(object, "lcd_title", s.lcd_title);
        cJSON_AddBoolToObject(object, "lcd_24_hour", s.lcd_24_hour);
        cJSON_AddBoolToObject(object, "lcd_pad_hour", s.lcd_pad_hour);
        cJSON_AddBoolToObject(object, "lcd_show_time_zone", s.lcd_show_time_zone);
        cJSON_AddStringToObject(object, "time_zone", s.time_zone);
        cJSON_AddStringToObject(object, "led_type", s.led_type == web_led_type_t::ky016 ? "ky016" : s.led_type == web_led_type_t::ws2812 ? "ws2812" : "none");
        cJSON_AddNumberToObject(object, "led_brightness", s.led_brightness_percent);
        cJSON_AddNumberToObject(object, "led_red_pin", s.led_red_pin);
        cJSON_AddNumberToObject(object, "led_green_pin", s.led_green_pin);
        cJSON_AddNumberToObject(object, "led_blue_pin", s.led_blue_pin);
        cJSON_AddNumberToObject(object, "led_ws2812_pin", s.led_ws2812_pin);
        cJSON *colours = cJSON_AddObjectToObject(object, "led_colors");
        for (size_t i = 0; i < WEB_LED_STATE_COUNT; ++i)
        {
            char hex[8];
            snprintf(hex, sizeof(hex), "#%06lX", static_cast<unsigned long>(s.led_rgb[i] & 0xFFFFFFu));
            cJSON_AddStringToObject(colours, s_led_state_names[i], hex);
        }
        cJSON_AddBoolToObject(object, "mqtt_enabled", s.mqtt_enabled);
        cJSON_AddStringToObject(object, "mqtt_uri", s.mqtt_uri);
        cJSON_AddStringToObject(object, "mqtt_username", s.mqtt_username);
        // Secrets are never sent: only whether they are set
        cJSON_AddBoolToObject(object, "mqtt_password_set", s.mqtt_password[0] != '\0');
        cJSON_AddStringToObject(object, "mqtt_topic", s.mqtt_topic);
        cJSON_AddNumberToObject(object, "mqtt_qos", s.mqtt_qos);
        cJSON_AddBoolToObject(object, "mqtt_retain", s.mqtt_retain);
        cJSON_AddNumberToObject(object, "mqtt_report_period", s.mqtt_report_period_s);
        cJSON_AddBoolToObject(object, "mqtt_client_reporting", s.mqtt_client_reporting);
        cJSON_AddBoolToObject(object, "mqtt_memory_reporting", s.mqtt_memory_reporting);
        cJSON_AddBoolToObject(object, "mqtt_historical_reporting", s.mqtt_historical_reporting);
        cJSON_AddBoolToObject(object, "mqtt_distinguish_auth", s.mqtt_distinguish_auth);
        cJSON_AddNumberToObject(object, "mqtt_tf_client_limit", s.mqtt_tf_client_limit);
        cJSON_AddNumberToObject(object, "tf_cmd_pin", s.tf_cmd_pin);
        cJSON_AddNumberToObject(object, "tf_clk_pin", s.tf_clk_pin);
        cJSON_AddNumberToObject(object, "tf_d0_pin", s.tf_d0_pin);
        cJSON_AddNumberToObject(object, "tf_d1_pin", s.tf_d1_pin);
        cJSON_AddNumberToObject(object, "tf_d2_pin", s.tf_d2_pin);
        cJSON_AddNumberToObject(object, "tf_d3_pin", s.tf_d3_pin);
        cJSON_AddBoolToObject(object, "ntp_auth_enabled", s.ntp_auth_enabled);
        cJSON_AddBoolToObject(object, "ntp_key_override", s.ntp_key_override);
        // The NTP keys are shown on the page (they have to be entered on the clients); the other secrets are not
        add_ntp_keys(cJSON_AddArrayToObject(object, "ntp_keys"), s.ntp_keys, s.ntp_key_count);
        cJSON_AddBoolToObject(object, "ote_enabled", s.ote_enabled);
    }

    bool json_string(cJSON *root, const char *name, char *out, size_t size, char *err, size_t err_size)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        if (item == nullptr)
            return true; // not sent: keep
        if (!cJSON_IsString(item) || item->valuestring == nullptr || strlen(item->valuestring) >= size)
        {
            snprintf(err, err_size, "Field %s: text of at most %u characters expected", name, static_cast<unsigned>(size - 1));
            return false;
        }
        snprintf(out, size, "%s", item->valuestring);
        return true;
    }

    bool json_bool(cJSON *root, const char *name, bool &out, char *err, size_t err_size)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        if (item == nullptr)
            return true;
        if (!cJSON_IsBool(item))
        {
            snprintf(err, err_size, "Field %s: true or false expected", name);
            return false;
        }
        out = cJSON_IsTrue(item);
        return true;
    }

    template <typename T>
    bool json_int(cJSON *root, const char *name, T &out, long min_value, long max_value, char *err, size_t err_size)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        if (item == nullptr)
            return true;
        const double value = cJSON_IsNumber(item) ? item->valuedouble : -1e30;
        if (!cJSON_IsNumber(item) || value != static_cast<double>(static_cast<long>(value)) || value < min_value || value > max_value)
        {
            snprintf(err, err_size, "Field %s: whole number from %ld to %ld expected", name, min_value, max_value);
            return false;
        }
        out = static_cast<T>(static_cast<long>(value));
        return true;
    }

    // Builds a candidate from the running settings and the fields of the request
    bool settings_from_json(cJSON *root, const web_settings_t &base, web_settings_t &s, char *err, size_t err_size)
    {
        s = base;
        bool ok = json_string(root, "fqdn", s.fqdn, sizeof(s.fqdn), err, err_size) &&
                  json_bool(root, "ipv4_dhcp", s.ipv4_dhcp, err, err_size) &&
                  json_string(root, "ipv4_address", s.ipv4_address, sizeof(s.ipv4_address), err, err_size) &&
                  json_int(root, "ipv4_prefix", s.ipv4_prefix, 0, 32, err, err_size) &&
                  json_string(root, "ipv4_gateway", s.ipv4_gateway, sizeof(s.ipv4_gateway), err, err_size) &&
                  json_bool(root, "ipv6_slaac", s.ipv6_slaac, err, err_size) &&
                  json_string(root, "ipv6_address", s.ipv6_address, sizeof(s.ipv6_address), err, err_size) &&
                  json_int(root, "ipv6_prefix", s.ipv6_prefix, 0, 128, err, err_size) &&
                  json_string(root, "ipv6_gateway", s.ipv6_gateway, sizeof(s.ipv6_gateway), err, err_size) &&
                  json_int(root, "ntp_client_rate_limit", s.ntp_client_rate_limit, 0, WEB_NTP_RATE_LIMIT_MAX, err, err_size) &&
                  json_int(root, "gnss_tx_pin", s.gnss_tx_pin, -1, 54, err, err_size) &&
                  json_int(root, "gnss_rx_pin", s.gnss_rx_pin, -1, 54, err, err_size) &&
                  json_int(root, "gnss_pps_pin", s.gnss_pps_pin, -1, 54, err, err_size) &&
                  json_int(root, "gnss_baud", s.gnss_baud, 0, 921600, err, err_size) &&
                  json_int(root, "ptp_loopback_pin", s.ptp_loopback_pin, -1, 54, err, err_size) &&
                  json_int(root, "pps_holdover", s.pps_holdover_s, 0, WEB_PPS_HOLDOVER_MAX_S, err, err_size) &&
                  json_int(root, "pps_lost_stratum", s.pps_lost_stratum, 2, 16, err, err_size) &&
                  json_bool(root, "pps_lost_serve", s.pps_lost_serve, err, err_size) &&
                  json_int(root, "pps_lost_serve_min", s.pps_lost_serve_min, 0, WEB_PPS_LOST_SERVE_MAX_MIN, err, err_size) &&
                  json_int(root, "pps_dispersion_ppm", s.pps_dispersion_ppm, 1, WEB_PPS_DISPERSION_MAX_PPM, err, err_size) &&
                  json_int(root, "gnss_min_fix_age", s.gnss_min_fix_age_s, 0, WEB_GNSS_MIN_FIX_AGE_MAX_S, err, err_size) &&
                  json_int(root, "gnss_min_satellites", s.gnss_min_satellites, 0, WEB_GNSS_MIN_SATELLITES_MAX, err, err_size) &&
                  json_bool(root, "lcd_enabled", s.lcd_enabled, err, err_size) &&
                  json_string(root, "lcd_title", s.lcd_title, sizeof(s.lcd_title), err, err_size) &&
                  json_bool(root, "lcd_24_hour", s.lcd_24_hour, err, err_size) &&
                  json_bool(root, "lcd_pad_hour", s.lcd_pad_hour, err, err_size) &&
                  json_bool(root, "lcd_show_time_zone", s.lcd_show_time_zone, err, err_size) &&
                  json_string(root, "time_zone", s.time_zone, sizeof(s.time_zone), err, err_size) &&
                  json_int(root, "led_brightness", s.led_brightness_percent, 0, 100, err, err_size) &&
                  json_int(root, "led_red_pin", s.led_red_pin, -1, 54, err, err_size) &&
                  json_int(root, "led_green_pin", s.led_green_pin, -1, 54, err, err_size) &&
                  json_int(root, "led_blue_pin", s.led_blue_pin, -1, 54, err, err_size) &&
                  json_int(root, "led_ws2812_pin", s.led_ws2812_pin, -1, 54, err, err_size) &&
                  json_bool(root, "mqtt_enabled", s.mqtt_enabled, err, err_size) &&
                  json_string(root, "mqtt_uri", s.mqtt_uri, sizeof(s.mqtt_uri), err, err_size) &&
                  json_string(root, "mqtt_username", s.mqtt_username, sizeof(s.mqtt_username), err, err_size) &&
                  json_string(root, "mqtt_topic", s.mqtt_topic, sizeof(s.mqtt_topic), err, err_size) &&
                  json_int(root, "mqtt_qos", s.mqtt_qos, 0, 2, err, err_size) &&
                  json_bool(root, "mqtt_retain", s.mqtt_retain, err, err_size) &&
                  json_int(root, "mqtt_report_period", s.mqtt_report_period_s, 30, 86400, err, err_size) &&
                  json_bool(root, "mqtt_client_reporting", s.mqtt_client_reporting, err, err_size) &&
                  json_bool(root, "mqtt_memory_reporting", s.mqtt_memory_reporting, err, err_size) &&
                  json_bool(root, "mqtt_historical_reporting", s.mqtt_historical_reporting, err, err_size) &&
                  json_bool(root, "mqtt_distinguish_auth", s.mqtt_distinguish_auth, err, err_size) &&
                  json_int(root, "mqtt_tf_client_limit", s.mqtt_tf_client_limit, WEB_MQTT_TF_CLIENT_LIMIT_MIN, WEB_MQTT_TF_CLIENT_LIMIT_MAX, err, err_size) &&
                  json_int(root, "tf_cmd_pin", s.tf_cmd_pin, 0, 54, err, err_size) &&
                  json_int(root, "tf_clk_pin", s.tf_clk_pin, 0, 54, err, err_size) &&
                  json_int(root, "tf_d0_pin", s.tf_d0_pin, 0, 54, err, err_size) &&
                  json_int(root, "tf_d1_pin", s.tf_d1_pin, 0, 54, err, err_size) &&
                  json_int(root, "tf_d2_pin", s.tf_d2_pin, 0, 54, err, err_size) &&
                  json_int(root, "tf_d3_pin", s.tf_d3_pin, 0, 54, err, err_size) &&
                  json_bool(root, "ntp_auth_enabled", s.ntp_auth_enabled, err, err_size) &&
                  json_bool(root, "ote_enabled", s.ote_enabled, err, err_size);
        if (!ok)
            return false;

        // MAC: "" or the hardware MAC = the hardware MAC (stored as ""), otherwise normalised to lower case with ':'
        char mac[WEB_MAC_TEXT_SIZE * 2] = "";
        bool mac_sent = cJSON_GetObjectItemCaseSensitive(root, "mac_address") != nullptr;
        if (!json_string(root, "mac_address", mac, sizeof(mac), err, err_size))
            return false;
        if (mac_sent)
        {
            char hardware[WEB_MAC_TEXT_SIZE];
            char normalised[WEB_MAC_TEXT_SIZE];
            if (mac[0] == '\0')
                s.mac_address[0] = '\0';
            else if (!web_normalise_mac(mac, normalised))
            {
                snprintf(err, err_size, "MAC address: xx:xx:xx:xx:xx:xx (hexadecimal) expected");
                return false;
            }
            else if (web_hardware_mac(hardware) && strcmp(hardware, normalised) == 0)
                s.mac_address[0] = '\0';
            else
                snprintf(s.mac_address, sizeof(s.mac_address), "%s", normalised);
        }

        cJSON *protocol = cJSON_GetObjectItemCaseSensitive(root, "gnss_protocol");
        if (protocol != nullptr)
        {
            if (cJSON_IsString(protocol) && strcmp(protocol->valuestring, "ubx") == 0)
                s.gnss_ublox = true;
            else if (cJSON_IsString(protocol) && strcmp(protocol->valuestring, "nmea") == 0)
                s.gnss_ublox = false;
            else
            {
                snprintf(err, err_size, "Field gnss_protocol: ubx or nmea expected");
                return false;
            }
        }

        cJSON *colours = cJSON_GetObjectItemCaseSensitive(root, "led_colors");
        if (colours != nullptr)
        {
            if (!cJSON_IsObject(colours))
            {
                snprintf(err, err_size, "Field led_colors: object expected");
                return false;
            }
            for (size_t i = 0; i < WEB_LED_STATE_COUNT; ++i)
            {
                cJSON *colour = cJSON_GetObjectItemCaseSensitive(colours, s_led_state_names[i]);
                if (colour == nullptr)
                    continue;
                const char *text = cJSON_IsString(colour) ? colour->valuestring : nullptr;
                bool valid = text != nullptr && text[0] == '#' && strlen(text) == 7;
                for (size_t k = 1; valid && k < 7; ++k)
                    valid = isxdigit(static_cast<unsigned char>(text[k])) != 0;
                if (!valid)
                {
                    snprintf(err, err_size, "LED colour %s: #RRGGBB expected", s_led_state_names[i]);
                    return false;
                }
                s.led_rgb[i] = static_cast<uint32_t>(strtoul(text + 1, nullptr, 16));
            }
        }

        cJSON *led = cJSON_GetObjectItemCaseSensitive(root, "led_type");
        if (led != nullptr)
        {
            if (cJSON_IsString(led) && strcmp(led->valuestring, "none") == 0)
                s.led_type = web_led_type_t::none;
            else if (cJSON_IsString(led) && strcmp(led->valuestring, "ky016") == 0)
                s.led_type = web_led_type_t::ky016;
            else if (cJSON_IsString(led) && strcmp(led->valuestring, "ws2812") == 0)
                s.led_type = web_led_type_t::ws2812;
            else
            {
                snprintf(err, err_size, "Field led_type: none, ky016 or ws2812 expected");
                return false;
            }
        }

        cJSON *dns = cJSON_GetObjectItemCaseSensitive(root, "dns");
        if (dns != nullptr)
        {
            if (!cJSON_IsArray(dns) || cJSON_GetArraySize(dns) > static_cast<int>(WEB_DNS_SERVER_COUNT))
            {
                snprintf(err, err_size, "At most %u DNS servers", static_cast<unsigned>(WEB_DNS_SERVER_COUNT));
                return false;
            }
            memset(s.dns, 0, sizeof(s.dns));
            size_t index = 0;
            cJSON *item = nullptr;
            cJSON_ArrayForEach(item, dns)
            {
                if (!cJSON_IsString(item) || strlen(item->valuestring) >= WEB_IPV6_TEXT_SIZE)
                {
                    snprintf(err, err_size, "DNS server %u is not valid", static_cast<unsigned>(index + 1));
                    return false;
                }
                snprintf(s.dns[index++], WEB_IPV6_TEXT_SIZE, "%s", item->valuestring);
            }
        }

        // Secrets: an empty field keeps the current value, a separate flag clears it
        char secret[WEB_MQTT_CREDENTIAL_SIZE] = "";
        bool clear = false;
        if (!json_string(root, "mqtt_password", secret, sizeof(secret), err, err_size) ||
            !json_bool(root, "mqtt_password_clear", clear, err, err_size))
            return false;
        if (clear)
            memset(s.mqtt_password, 0, sizeof(s.mqtt_password));
        else if (secret[0] != '\0')
            snprintf(s.mqtt_password, sizeof(s.mqtt_password), "%s", secret);

        // NTP keys: "ntp_keys" (list of {id, key}) replaces the keys and switches to the keys of the web settings,
        // "ntp_key_clear" returns to the compiled keys; neither: unchanged
        clear = false;
        if (!json_bool(root, "ntp_key_clear", clear, err, err_size))
            return false;
        cJSON *keys = cJSON_GetObjectItemCaseSensitive(root, "ntp_keys");
        if (clear)
        {
            s.ntp_key_override = false;
            s.ntp_key_count = 0;
            memset(s.ntp_keys, 0, sizeof(s.ntp_keys));
        }
        else if (keys != nullptr)
        {
            if (!cJSON_IsArray(keys) || cJSON_GetArraySize(keys) > static_cast<int>(WEB_NTP_MAX_KEYS))
            {
                snprintf(err, err_size, "At most %u NTP keys", static_cast<unsigned>(WEB_NTP_MAX_KEYS));
                return false;
            }
            web_ntp_key_t list[WEB_NTP_MAX_KEYS] = {};
            size_t count = 0;
            cJSON *item = nullptr;
            cJSON_ArrayForEach(item, keys)
            {
                cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
                cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "key");
                if (!cJSON_IsObject(item) || !cJSON_IsNumber(id) || !cJSON_IsString(value) || id->valuedouble < 1 ||
                    id->valuedouble > 65535 || id->valuedouble != static_cast<double>(static_cast<long>(id->valuedouble)) ||
                    strlen(value->valuestring) >= WEB_NTP_KEY_SIZE)
                {
                    memset(list, 0, sizeof(list));
                    snprintf(err, err_size, "NTP key %u: key ID 1 to 65535 and 32 letters or digits expected", static_cast<unsigned>(count + 1));
                    return false;
                }
                list[count].id = static_cast<uint32_t>(id->valuedouble);
                snprintf(list[count].key, sizeof(list[count].key), "%s", value->valuestring);
                ++count;
            }
            s.ntp_key_override = true;
            s.ntp_key_count = static_cast<uint8_t>(count);
            memcpy(s.ntp_keys, list, sizeof(list));
            memset(list, 0, sizeof(list));
        }
        memset(secret, 0, sizeof(secret));
        return true;
    }

    // ---- Restart and network confirmation ----
    void restart_task(void *parameter)
    {
        vTaskDelay(pdMS_TO_TICKS(1500)); // let the response reach the browser
        app_controlled_restart(static_cast<const char *>(parameter));
        vTaskDelete(nullptr);
    }

    void schedule_restart(const char *reason)
    {
        if (xTaskCreatePinnedToCore(restart_task, "web_restart", 4096, const_cast<char *>(reason), TASK_PRIORITY, nullptr, TASK_CORE) != pdPASS)
            app_controlled_restart(reason);
    }

    // Server task: the confirmation time is over
    void network_timeout_work(void *)
    {
        if (s_pending == nullptr || !s_pending->active || esp_timer_get_time() < s_pending->deadline_us)
            return;
        s_pending->active = false;
        if (s_pending->restart_on_timeout)
        {
            ESP_LOGW(TAG, "Network settings were not confirmed within 120 s: restarting with the saved settings");
            schedule_restart("web_admin_network_not_confirmed");
            return;
        }
        ESP_LOGW(TAG, "Network settings were not confirmed within 120 s: the previous settings are restored");
        web_settings_t running{};
        web_settings_get_running(running);
        web_settings_t restored = running;
        // only the network part returns to the previous values
        restored.ipv4_dhcp = s_pending->previous.ipv4_dhcp;
        memcpy(restored.ipv4_address, s_pending->previous.ipv4_address, sizeof(restored.ipv4_address));
        restored.ipv4_prefix = s_pending->previous.ipv4_prefix;
        memcpy(restored.ipv4_gateway, s_pending->previous.ipv4_gateway, sizeof(restored.ipv4_gateway));
        restored.ipv6_slaac = s_pending->previous.ipv6_slaac;
        memcpy(restored.ipv6_address, s_pending->previous.ipv6_address, sizeof(restored.ipv6_address));
        restored.ipv6_prefix = s_pending->previous.ipv6_prefix;
        memcpy(restored.ipv6_gateway, s_pending->previous.ipv6_gateway, sizeof(restored.ipv6_gateway));
        memcpy(restored.dns, s_pending->previous.dns, sizeof(restored.dns));
        web_net_apply(app_eth_netif(), restored, &running);
        web_settings_set_running(restored);
        app_network_settings_changed();
    }

    void confirm_timer_callback(void *)
    {
        if (s_server != nullptr)
            httpd_queue_work(s_server, network_timeout_work, nullptr);
    }

    void start_network_confirmation(const web_settings_t &previous, const web_settings_t &applied, bool restart_on_timeout)
    {
        s_pending->active = true;
        s_pending->restart_on_timeout = restart_on_timeout;
        s_pending->deadline_us = esp_timer_get_time() + NETWORK_CONFIRM_TIMEOUT_US;
        s_pending->previous = previous;
        s_pending->applied = applied;
        esp_timer_stop(s_confirm_timer);
        esp_timer_start_once(s_confirm_timer, NETWORK_CONFIRM_TIMEOUT_US + 100000LL);
    }

    void add_confirm_info(cJSON *object)
    {
        cJSON *pending = cJSON_AddObjectToObject(object, "network_pending");
        const bool active = s_pending != nullptr && s_pending->active;
        cJSON_AddBoolToObject(pending, "active", active);
        if (!active)
            return;
        const int64_t left_us = s_pending->deadline_us - esp_timer_get_time();
        cJSON_AddNumberToObject(pending, "seconds_left", left_us > 0 ? static_cast<double>(left_us / 1000000LL) : 0.0);
        cJSON *urls = cJSON_AddArrayToObject(pending, "urls");
        char url[80];
        if (!s_pending->applied.ipv4_dhcp)
        {
            snprintf(url, sizeof(url), "https://%s/", s_pending->applied.ipv4_address);
            cJSON_AddItemToArray(urls, cJSON_CreateString(url));
        }
        if (s_pending->applied.ipv6_address[0] != '\0')
        {
            snprintf(url, sizeof(url), "https://[%s]/", s_pending->applied.ipv6_address);
            cJSON_AddItemToArray(urls, cJSON_CreateString(url));
        }
    }

    // ---- Handlers ----
    esp_err_t handle_index(httpd_req_t *req) { return send_static(req, "text/html; charset=utf-8", index_html_start, index_html_end); }
    esp_err_t handle_js(httpd_req_t *req) { return send_static(req, "text/javascript; charset=utf-8", app_js_start, app_js_end); }
    esp_err_t handle_css(httpd_req_t *req) { return send_static(req, "text/css; charset=utf-8", app_css_start, app_css_end); }
    // German texts of the page (English is the source text in index.html and app.js)
    esp_err_t handle_i18n(httpd_req_t *req) { return send_static(req, "text/javascript; charset=utf-8", i18n_js_start, i18n_js_end); }

    // Binary file (EMBED_FILES, no NUL appended), may be cached by the browser for a day
    esp_err_t handle_favicon(httpd_req_t *req)
    {
        set_security_headers(req, "max-age=86400");
        httpd_resp_set_type(req, "image/x-icon");
        return httpd_resp_send(req, favicon_ico_start, favicon_ico_end - favicon_ico_start);
    }

    esp_err_t handle_not_found(httpd_req_t *req, httpd_err_code_t)
    {
        return send_error(req, "404 Not Found", "Not found");
    }

    esp_err_t handle_login(httpd_req_t *req)
    {
        struct sockaddr_storage peer{};
        if (!peer_address(req, peer, false))
            return send_error(req, "400 Bad Request", "No peer address");
        if (!same_origin(req))
            return send_error(req, "403 Forbidden", "Request rejected (origin)");
        const uint32_t wait_s = web_auth_login_wait_seconds(peer);
        if (wait_s > 0)
        {
            char retry[16];
            snprintf(retry, sizeof(retry), "%lu", static_cast<unsigned long>(wait_s));
            httpd_resp_set_hdr(req, "Retry-After", retry);
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "error", "Too many failed logins, please wait");
            cJSON_AddNumberToObject(root, "retry_after", wait_s);
            return send_json(req, "429 Too Many Requests", root);
        }
        cJSON *root = read_json(req);
        if (root == nullptr)
            return ESP_OK;
        cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        const bool valid_type = cJSON_IsString(password) && password->valuestring != nullptr;
        const size_t length = valid_type ? strlen(password->valuestring) : 0;
        const bool ok = valid_type && length <= WEB_PASSWORD_MAX_LEN && web_auth_check_password(peer, password->valuestring, length);
        if (valid_type)
            memset(password->valuestring, 0, length);
        cJSON_Delete(root);
        if (!ok)
        {
            char address[INET6_ADDRSTRLEN];
            format_address(peer, address, sizeof(address));
            ESP_LOGW(TAG, "Web admin: failed login from %s", address);
            return send_error(req, "401 Unauthorized", "Wrong password");
        }

        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        char csrf[WEB_CSRF_TOKEN_HEX_LEN + 1];
        if (!web_auth_create_session(token, csrf))
            return send_error(req, "500 Internal Server Error", "Session could not be created");
        char cookie[WEB_SESSION_TOKEN_HEX_LEN + 80];
        snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; Secure; HttpOnly; SameSite=Strict", SESSION_COOKIE, token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "csrf", csrf);
        const esp_err_t err = send_json(req, "200 OK", reply);
        memset(token, 0, sizeof(token));
        memset(cookie, 0, sizeof(cookie));
        return err;
    }

    esp_err_t handle_logout(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1] = "";
        if (session_cookie(req, token))
        {
            char csrf[WEB_CSRF_TOKEN_HEX_LEN + 2];
            if (same_origin(req) && header(req, "X-CSRF-Token", csrf, sizeof(csrf)) && web_auth_csrf_valid(token, csrf))
                web_auth_end_session(token);
        }
        httpd_resp_set_hdr(req, "Set-Cookie", "__Host-sid=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
        return send_ok(req, nullptr);
    }

    esp_err_t handle_state(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, false, token) != gate_t::passed)
            return ESP_OK;
        char csrf[WEB_CSRF_TOKEN_HEX_LEN + 1] = "";
        web_auth_session_valid(token, csrf);

        // web_settings_t is about 1 kB: on the heap, not on the server task stack
        struct sets_t
        {
            web_settings_t running, defaults, saved;
            web_net_status_t net;
            web_app_status_t app;
        };
        sets_t *sets = new (std::nothrow) sets_t{};
        if (sets == nullptr)
            return send_error(req, "503 Service Unavailable", "Out of memory");
        web_settings_get_running(sets->running);
        web_settings_defaults(sets->defaults);
        const bool saved = web_settings_get_saved(sets->saved);
        web_net_status(app_eth_netif(), sets->running, sets->net);
        app_get_status(sets->app);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "csrf", csrf);
        cJSON_AddBoolToObject(root, "default_password", web_auth_default_password_active());
        cJSON_AddStringToObject(root, "fingerprint", s_identity.fingerprint);
        settings_to_json(cJSON_AddObjectToObject(root, "settings"), sets->running);
        settings_to_json(cJSON_AddObjectToObject(root, "defaults"), sets->defaults);
        // Keys of ESP32TimeServerSettingsSecrets.h (shown while no keys are set on the page)
        cJSON *compiled = cJSON_AddArrayToObject(root, "ntp_compiled_keys");
        for (const symmetric_key_t &key : symmetric_keys)
        {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", key.key_id);
            char value[WEB_NTP_KEY_SIZE];
            snprintf(value, sizeof(value), "%.*s", static_cast<int>(sizeof(key.key_value)), key.key_value);
            cJSON_AddStringToObject(item, "key", value);
            cJSON_AddItemToArray(compiled, item);
        }
        if (saved)
            settings_to_json(cJSON_AddObjectToObject(root, "saved"), sets->saved);
        else
            cJSON_AddNullToObject(root, "saved");
        bool network_confirm = false;
        cJSON_AddBoolToObject(root, "oneshot_boot", web_settings_boot_was_oneshot(&network_confirm));
        // Settings in use differ from what the next start would load (saved settings, else the compiled defaults)
        cJSON_AddBoolToObject(root, "unsaved_changes", !web_settings_equal(sets->running, saved ? sets->saved : sets->defaults));
        add_confirm_info(root);

        cJSON *allow = cJSON_AddArrayToObject(root, "allow_list");
        web_allow_entry_t entries[WEB_ALLOW_LIST_MAX];
        const size_t count = web_auth_allow_list(entries, WEB_ALLOW_LIST_MAX);
        for (size_t i = 0; i < count; ++i)
        {
            char text[64];
            web_auth_format_cidr(entries[i], text, sizeof(text));
            cJSON_AddItemToArray(allow, cJSON_CreateString(text));
        }

        cJSON *status = cJSON_AddObjectToObject(root, "status");
        struct sockaddr_storage peer{};
        char address[INET6_ADDRSTRLEN] = "";
        if (peer_address(req, peer, false))
            format_address(peer, address, sizeof(address));
        cJSON_AddStringToObject(status, "client", address);
        cJSON_AddBoolToObject(status, "ipv4_dhcp_running", sets->net.ipv4_dhcp_running);
        cJSON_AddStringToObject(status, "ipv4_address", sets->net.ipv4_address);
        cJSON_AddNumberToObject(status, "ipv4_prefix", sets->net.ipv4_prefix);
        cJSON_AddStringToObject(status, "ipv4_gateway", sets->net.ipv4_gateway);
        cJSON *ipv6 = cJSON_AddArrayToObject(status, "ipv6");
        for (size_t i = 0; i < sets->net.ipv6_count; ++i)
            cJSON_AddItemToArray(ipv6, cJSON_CreateString(sets->net.ipv6[i]));
        cJSON *dns = cJSON_AddArrayToObject(status, "dns");
        for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
            if (sets->net.dns[i][0] != '\0')
                cJSON_AddItemToArray(dns, cJSON_CreateString(sets->net.dns[i]));
        cJSON *dns_v4 = cJSON_AddArrayToObject(status, "dns_received_v4");
        for (size_t i = 0; i < sets->net.dns_received_v4_count; ++i)
            cJSON_AddItemToArray(dns_v4, cJSON_CreateString(sets->net.dns_received_v4[i]));
        cJSON *dns_v6 = cJSON_AddArrayToObject(status, "dns_received_v6");
        for (size_t i = 0; i < sets->net.dns_received_v6_count; ++i)
            cJSON_AddItemToArray(dns_v6, cJSON_CreateString(sets->net.dns_received_v6[i]));
        cJSON_AddStringToObject(status, "firmware", sets->app.firmware_version);
        cJSON_AddStringToObject(status, "build", sets->app.build);
        cJSON_AddBoolToObject(status, "firmware_check_pending", sets->app.firmware_check_pending);
        cJSON_AddStringToObject(status, "chip_revision", sets->app.chip_revision);
        cJSON_AddBoolToObject(status, "ptp_loopback_supported", sets->app.ptp_loopback_supported);
        cJSON_AddNumberToObject(status, "uptime_s", static_cast<double>(esp_timer_get_time() / 1000000LL));
        char now_text[32] = "";
        const time_t now = time(nullptr);
        if (sets->app.time_set)
        {
            struct tm utc{};
            gmtime_r(&now, &utc);
            strftime(now_text, sizeof(now_text), "%Y-%m-%dT%H:%M:%SZ", &utc);
        }
        cJSON_AddStringToObject(status, "utc", now_text);
        cJSON_AddBoolToObject(status, "time_set", sets->app.time_set);
        cJSON_AddBoolToObject(status, "pps_active", sets->app.pps_active);
        cJSON_AddBoolToObject(status, "gnss_locked", sets->app.gnss_locked);
        cJSON_AddBoolToObject(status, "stratum_one", sets->app.stratum_one);
        cJSON_AddBoolToObject(status, "pps_holdover", sets->app.pps_holdover);
        cJSON_AddBoolToObject(status, "gnss_fix_waiting", sets->app.gnss_fix_waiting);
        cJSON_AddNumberToObject(status, "gnss_fix_age_s", sets->app.gnss_fix_age_s);
        cJSON_AddBoolToObject(status, "ublox_receiver", sets->app.ublox_receiver);
        cJSON_AddNumberToObject(status, "gnss_baud_in_use", sets->app.gnss_baud_in_use);
        cJSON_AddStringToObject(status, "gnss_dynamic_model", sets->app.gnss_dynamic_model);
        cJSON_AddNumberToObject(status, "gnss_satellites_used", sets->app.gnss_satellites_used);
        cJSON_AddNumberToObject(status, "gnss_fix_quality", sets->app.gnss_fix_quality);
        cJSON_AddNumberToObject(status, "gnss_rmc_after_pps_ms", sets->app.gnss_rmc_after_pps_ms);
        cJSON_AddNumberToObject(status, "gnss_rmc_after_pps_min_ms", sets->app.gnss_rmc_after_pps_min_ms);
        cJSON_AddNumberToObject(status, "gnss_rmc_after_pps_max_ms", sets->app.gnss_rmc_after_pps_max_ms);
        cJSON_AddNumberToObject(status, "gnss_pvt_satellites", sets->app.gnss_pvt_satellites);
        cJSON_AddNumberToObject(status, "gnss_qerr_state", sets->app.gnss_qerr_state);
        cJSON_AddNumberToObject(status, "gnss_qerr_slope", sets->app.gnss_qerr_slope_milli / 1000.0);
        cJSON_AddBoolToObject(status, "gnss_rf_known", sets->app.gnss_rf_known);
        cJSON_AddNumberToObject(status, "gnss_antenna_status", sets->app.gnss_antenna_status);
        cJSON_AddNumberToObject(status, "gnss_antenna_power", sets->app.gnss_antenna_power);
        cJSON_AddNumberToObject(status, "gnss_jamming_state", sets->app.gnss_jamming_state);
        cJSON_AddNumberToObject(status, "gnss_agc_percent", sets->app.gnss_agc_percent);
        cJSON_AddBoolToObject(status, "lcd_present", sets->app.lcd_present);
        cJSON_AddBoolToObject(status, "led_active", sets->app.led_active);
        cJSON_AddBoolToObject(status, "mqtt_connected", sets->app.mqtt_connected);
        cJSON_AddBoolToObject(status, "mqtt_tf_queue", sets->app.mqtt_tf_queue);
        char hardware_mac[WEB_MAC_TEXT_SIZE] = "";
        web_hardware_mac(hardware_mac);
        cJSON_AddStringToObject(status, "mac_hardware", hardware_mac);
        cJSON_AddStringToObject(status, "mac_address", sets->app.mac_address);
        cJSON_AddBoolToObject(status, "ntp_auth_available", sets->app.ntp_auth_available);
        cJSON_AddNumberToObject(status, "ntp_auth_compiled_keys", sets->app.ntp_auth_compiled_keys);
        cJSON_AddNumberToObject(status, "ntp_auth_keys_in_use", sets->app.ntp_auth_keys_in_use);
        cJSON_AddNumberToObject(status, "ntp_rate_limited", sets->app.ntp_rate_limited_requests);
        cJSON_AddNumberToObject(status, "free_heap", sets->app.free_heap);
        delete sets;
        return send_json(req, "200 OK", root);
    }

    // Apply: live settings take effect now (network changes have to be confirmed within 120 s), settings that need
    // a restart are written as a one-shot set and the device restarts
    esp_err_t handle_apply(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        if (s_pending->active)
            return send_error(req, "409 Conflict", "Confirm or wait for the pending network change first");
        cJSON *root = read_json(req);
        if (root == nullptr)
            return ESP_OK;

        struct sets_t
        {
            web_settings_t running, candidate;
        };
        sets_t *sets = new (std::nothrow) sets_t{};
        if (sets == nullptr)
        {
            cJSON_Delete(root);
            return send_error(req, "503 Service Unavailable", "Out of memory");
        }
        web_settings_get_running(sets->running);
        char err[128] = "";
        const bool parsed = settings_from_json(root, sets->running, sets->candidate, err, sizeof(err));
        cJSON_Delete(root);
        if (!parsed || !web_settings_validate(sets->candidate, err, sizeof(err)))
        {
            delete sets;
            return send_error(req, "400 Bad Request", err);
        }

        const bool network_changed = web_settings_network_differs(sets->candidate, sets->running);
        cJSON *reply = cJSON_CreateObject();
        if (web_settings_restart_fields_differ(sets->candidate, sets->running))
        {
            const esp_err_t write_err = web_settings_save_oneshot(sets->candidate, network_changed);
            delete sets;
            cJSON_Delete(reply);
            if (write_err != ESP_OK)
                return send_error(req, "500 Internal Server Error", "One-shot settings could not be written");
            ESP_LOGI(TAG, "Web admin: settings applied, restarting with them once");
            cJSON *restart = cJSON_CreateObject();
            cJSON_AddBoolToObject(restart, "ok", true);
            cJSON_AddBoolToObject(restart, "restart", true);
            cJSON_AddBoolToObject(restart, "network_changed", network_changed);
            const esp_err_t send_err = send_json(req, "200 OK", restart);
            schedule_restart("web_admin_apply");
            return send_err;
        }

        if (network_changed)
        {
            if (!web_net_apply(app_eth_netif(), sets->candidate, &sets->running))
                ESP_LOGW(TAG, "Web admin: some network settings could not be applied");
            start_network_confirmation(sets->running, sets->candidate, false);
        }
        web_settings_set_running(sets->candidate);
        app_apply_live_settings(sets->candidate);
        if (network_changed)
            app_network_settings_changed();
        ESP_LOGI(TAG, "Web admin: settings applied%s", network_changed ? " (network: waiting for the confirmation)" : "");
        delete sets;

        cJSON_AddBoolToObject(reply, "ok", true);
        cJSON_AddBoolToObject(reply, "restart", false);
        cJSON_AddBoolToObject(reply, "network_changed", network_changed);
        add_confirm_info(reply);
        return send_json(req, "200 OK", reply);
    }

    esp_err_t handle_confirm(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        if (!s_pending->active)
            return send_error(req, "409 Conflict", "No network change to confirm");
        // With a new static address the confirmation has to come over that address
        struct sockaddr_storage local{};
        const bool has_new_static = !s_pending->applied.ipv4_dhcp || s_pending->applied.ipv6_address[0] != '\0';
        if (has_new_static && (!peer_address(req, local, true) || !web_net_is_static_address(s_pending->applied, local)))
            return send_error(req, "409 Conflict", "Open the page over the new static address and confirm there");
        s_pending->active = false;
        esp_timer_stop(s_confirm_timer);
        ESP_LOGI(TAG, "Web admin: network settings confirmed");
        return send_ok(req, "Network settings confirmed. Use Save to keep them after a restart.");
    }

    esp_err_t handle_save(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        if (s_pending->active)
            return send_error(req, "409 Conflict", "Confirm the pending network change first");
        cJSON *root = read_json(req);
        if (root == nullptr)
            return ESP_OK;
        struct sets_t
        {
            web_settings_t running, candidate;
        };
        sets_t *sets = new (std::nothrow) sets_t{};
        if (sets == nullptr)
        {
            cJSON_Delete(root);
            return send_error(req, "503 Service Unavailable", "Out of memory");
        }
        web_settings_get_running(sets->running);
        char err[128] = "";
        const bool parsed = settings_from_json(root, sets->running, sets->candidate, err, sizeof(err));
        cJSON_Delete(root);
        if (!parsed || !web_settings_validate(sets->candidate, err, sizeof(err)))
        {
            delete sets;
            return send_error(req, "400 Bad Request", err);
        }
        // Network settings are only saved once they work: apply and confirm them first (a lock-out would need
        // the factory reset otherwise)
        if (web_settings_network_differs(sets->candidate, sets->running))
        {
            delete sets;
            return send_error(req, "409 Conflict", "Apply and confirm the network settings before saving them");
        }
        const esp_err_t save_err = web_settings_save(sets->candidate);
        const bool equal = web_settings_equal(sets->candidate, sets->running);
        delete sets;
        if (save_err != ESP_OK)
            return send_error(req, "500 Internal Server Error", "Settings could not be written to NVS");
        ESP_LOGI(TAG, "Web admin: settings saved to NVS");
        return send_ok(req, equal ? "Settings saved. They are loaded at every start."
                                  : "Settings saved. Some saved values differ from the values in use: they take effect after Apply or a restart.");
    }

    esp_err_t handle_factory_reset(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        struct sockaddr_storage peer{};
        peer_address(req, peer, false);
        if (web_auth_login_wait_seconds(peer) > 0)
            return send_error(req, "429 Too Many Requests", "Too many failed password checks, please wait");
        cJSON *root = read_json(req);
        if (root == nullptr)
            return ESP_OK;
        cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        const bool ok = cJSON_IsString(password) && strlen(password->valuestring) <= WEB_PASSWORD_MAX_LEN &&
                        web_auth_verify_current_password(peer, password->valuestring, strlen(password->valuestring));
        if (cJSON_IsString(password))
            memset(password->valuestring, 0, strlen(password->valuestring));
        cJSON_Delete(root);
        if (!ok)
            return send_error(req, "403 Forbidden", "Wrong password");
        if (web_settings_factory_reset() != ESP_OK)
            return send_error(req, "500 Internal Server Error", "NVS could not be erased");
        web_auth_end_all_sessions();
        ESP_LOGW(TAG, "Web admin: factory reset (settings, password and allow-list erased), restarting");
        const esp_err_t err = send_ok(req, "Factory reset done. The device restarts with the compiled settings.");
        schedule_restart("web_admin_factory_reset");
        return err;
    }

    esp_err_t handle_password(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        struct sockaddr_storage peer{};
        peer_address(req, peer, false);
        if (web_auth_login_wait_seconds(peer) > 0)
            return send_error(req, "429 Too Many Requests", "Too many failed password checks, please wait");
        cJSON *root = read_json(req);
        if (root == nullptr)
            return ESP_OK;
        cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
        cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "new");
        esp_err_t result = ESP_FAIL;
        const char *message = "Wrong current password";
        const char *status = "403 Forbidden";
        if (cJSON_IsString(current) && cJSON_IsString(next) && strlen(current->valuestring) <= WEB_PASSWORD_MAX_LEN &&
            web_auth_verify_current_password(peer, current->valuestring, strlen(current->valuestring)))
        {
            const size_t length = strlen(next->valuestring);
            if (strcmp(next->valuestring, current->valuestring) == 0)
            {
                message = "The new password must differ from the current one";
                status = "400 Bad Request";
            }
            else
            {
                result = web_auth_set_password(next->valuestring, length, token);
                if (result == ESP_ERR_INVALID_ARG)
                {
                    message = "New password: 12 to 128 characters, no control characters";
                    status = "400 Bad Request";
                }
                else if (result != ESP_OK)
                {
                    message = "Password could not be stored";
                    status = "500 Internal Server Error";
                }
            }
        }
        if (cJSON_IsString(current))
            memset(current->valuestring, 0, strlen(current->valuestring));
        if (cJSON_IsString(next))
            memset(next->valuestring, 0, strlen(next->valuestring));
        cJSON_Delete(root);
        if (result != ESP_OK)
            return send_error(req, status, message);
        return send_ok(req, "Password changed and stored. All other sessions were closed.");
    }

    esp_err_t handle_reboot(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        ESP_LOGW(TAG, "Web admin: restart requested");
        const esp_err_t err = send_ok(req, "The device restarts now. Applied settings that were not saved are discarded.");
        schedule_restart("web_admin_reboot");
        return err;
    }

    // Restart once with Over The Ethernet updates on: the settings in use with ote_enabled = true are written as a
    // one-shot set. The restart after that start (e.g. the one after an upload) returns to the saved settings.
    esp_err_t handle_ote_once(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        if (s_pending->active)
            return send_error(req, "409 Conflict", "Confirm or wait for the pending network change first");
        web_settings_t *settings = new (std::nothrow) web_settings_t{}; // about 1 kB: not on the server task stack
        if (settings == nullptr)
            return send_error(req, "503 Service Unavailable", "Out of memory");
        web_settings_get_running(*settings);
        settings->ote_enabled = true;
        char err[128] = "";
        if (!web_settings_validate(*settings, err, sizeof(err)))
        {
            delete settings;
            return send_error(req, "400 Bad Request", err);
        }
        const esp_err_t write_err = web_settings_save_oneshot(*settings, false);
        delete settings;
        if (write_err != ESP_OK)
            return send_error(req, "500 Internal Server Error", "One-shot settings could not be written");
        ESP_LOGW(TAG, "Web admin: restart with Over The Ethernet updates on until the next restart");
        const esp_err_t send_err = send_ok(req, "The device restarts now and accepts firmware updates until its next restart.");
        schedule_restart("web_admin_ote_once");
        return send_err;
    }

    // Firmware upload (Over The Ethernet update, see web_ote.h). Two ways to authenticate:
    //  - the page: session cookie, same origin and the CSRF header, as every other state changing request
    //  - scripts (tools/ote_upload.py): HTTP Basic authentication with the web admin password (any user name),
    //    inside the TLS connection. Refused while the factory password is in use; wrong passwords count for the
    //    login throttling like failed logins.
    // The body is the raw application image (Content-Type application/octet-stream).
    esp_err_t handle_ote(httpd_req_t *req)
    {
        if (!app_ote_enabled())
            return send_error(req, "403 Forbidden", "Over The Ethernet updates are switched off in the settings");

        if (httpd_req_get_hdr_value_len(req, "Authorization") > 0)
        {
            struct sockaddr_storage peer{};
            if (!peer_address(req, peer, false))
                return send_error(req, "400 Bad Request", "No peer address");
            if (web_auth_default_password_active())
            {
                send_error(req, "403 Forbidden", "Scripted uploads are refused while the web admin password is the factory default");
                return ESP_FAIL; // closes the connection (the body is not read)
            }
            const uint32_t wait_s = web_auth_login_wait_seconds(peer);
            if (wait_s > 0)
            {
                char retry[16];
                snprintf(retry, sizeof(retry), "%lu", static_cast<unsigned long>(wait_s));
                httpd_resp_set_hdr(req, "Retry-After", retry);
                return send_error(req, "429 Too Many Requests", "Too many failed password checks, please wait");
            }
            // "Basic " and base64("<user name>:<password>"); the buffers are cleared after the check
            constexpr size_t CREDENTIALS_MAX = 64 + 1 + WEB_PASSWORD_MAX_LEN; // user name, ':', password
            char authorization[6 + 4 * ((CREDENTIALS_MAX + 2) / 3) + 8] = "";
            unsigned char credentials[CREDENTIALS_MAX] = {};
            const bool read = header(req, "Authorization", authorization, sizeof(authorization));
            if (read && strncasecmp(authorization, "Basic ", 6) != 0)
            {
                // Another scheme (e.g. "Bearer <OTE password>" of an older tools/ote_upload.py): no password checked
                memset(authorization, 0, sizeof(authorization));
                httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Time Server\", charset=\"UTF-8\"");
                send_error(req, "401 Unauthorized", "Use HTTP Basic authentication with the web admin password");
                return ESP_FAIL;
            }
            const char *encoded = authorization + 6;
            while (*encoded == ' ')
                ++encoded;
            size_t encoded_length = read ? strlen(encoded) : 0;
            while (encoded_length > 0 && encoded[encoded_length - 1] == ' ')
                --encoded_length;
            size_t credentials_length = 0;
            const bool decoded = read && encoded_length > 0 &&
                                 mbedtls_base64_decode(credentials, sizeof(credentials), &credentials_length,
                                                       reinterpret_cast<const unsigned char *>(encoded), encoded_length) == 0;
            const unsigned char *colon = decoded ? static_cast<const unsigned char *>(memchr(credentials, ':', credentials_length)) : nullptr;
            bool ok = false;
            if (colon != nullptr)
                ok = web_auth_check_password(peer, reinterpret_cast<const char *>(colon + 1),
                                             credentials_length - static_cast<size_t>(colon + 1 - credentials));
            else
                web_auth_record_failed_check(peer); // header too long or not decodable
            memset(authorization, 0, sizeof(authorization));
            memset(credentials, 0, sizeof(credentials));
            if (!ok)
            {
                char address[INET6_ADDRSTRLEN];
                format_address(peer, address, sizeof(address));
                ESP_LOGW(TAG, "Web admin: firmware upload with a wrong password from %s", address);
                httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Time Server\", charset=\"UTF-8\"");
                send_error(req, "401 Unauthorized", "Wrong password");
                return ESP_FAIL; // closes the connection (the body is not read)
            }
        }
        else
        {
            char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
            char csrf[WEB_CSRF_TOKEN_HEX_LEN + 2];
            if (!session_cookie(req, token) || !web_auth_session_valid(token, nullptr))
            {
                send_error(req, "401 Unauthorized", "Not logged in");
                return ESP_FAIL;
            }
            if (!origin_is_this_server(req) || !header(req, "X-CSRF-Token", csrf, sizeof(csrf)) || !web_auth_csrf_valid(token, csrf))
            {
                send_error(req, "403 Forbidden", "Request rejected (origin or CSRF token)");
                return ESP_FAIL;
            }
        }

        char content_type[48];
        if (!header(req, "Content-Type", content_type, sizeof(content_type)) || strcmp(content_type, "application/octet-stream") != 0)
        {
            send_error(req, "415 Unsupported Media Type", "Send the application image (.bin) as application/octet-stream");
            return ESP_FAIL;
        }

        const web_ote_result_t result = web_ote_receive(req);
        if (result.accepted)
            return send_ok(req, result.message);
        httpd_resp_set_hdr(req, "Connection", "close");
        send_error(req, result.status, result.message);
        // An unread rest of the image would otherwise be read (and thrown away) by the server
        return result.body_consumed ? ESP_OK : ESP_FAIL;
    }

    // Without login: only the names, for the header of the login page (the FQDN is also in the certificate, which
    // every client gets before the login)
    // Diagnostics panel (review 4.5): only while the panel is open; the stability takes some 10 ms
    esp_err_t handle_diag(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, false, token) != gate_t::passed)
            return ESP_OK;
        web_diag_t *d = new (std::nothrow) web_diag_t{};
        if (d == nullptr)
            return send_error(req, "503 Service Unavailable", "Out of memory");
        app_get_diag(*d);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "servo", d->servo);
        cJSON_AddBoolToObject(root, "servo_valid", d->servo_valid);
        cJSON_AddBoolToObject(root, "loopback_active", d->loopback_active);
        cJSON_AddBoolToObject(root, "from_loopback", d->from_loopback);
        cJSON_AddNumberToObject(root, "offset_ns", d->offset_ns);
        cJSON_AddNumberToObject(root, "noise_ns", d->noise_ns);
        cJSON_AddNumberToObject(root, "free_ppb", d->free_ppb);
        cJSON_AddNumberToObject(root, "cmd_ppb", d->cmd_ppb);
        cJSON_AddNumberToObject(root, "sw_bias_ns", d->sw_bias_ns);
        cJSON_AddNumberToObject(root, "sw_bias_samples", d->sw_bias_samples);
        cJSON_AddNumberToObject(root, "relock_residual_ns", d->relock_residual_ns);
        cJSON_AddNumberToObject(root, "loopback_used", d->loopback_used);
        cJSON_AddNumberToObject(root, "loopback_missing", d->loopback_missing);
        cJSON_AddNumberToObject(root, "loopback_rejected", d->loopback_rejected);
        cJSON_AddNumberToObject(root, "outliers", d->outliers);
        cJSON_AddNumberToObject(root, "steps", d->steps);
        cJSON_AddNumberToObject(root, "command_failures", d->command_failures);
        cJSON_AddNumberToObject(root, "holdover_running_s", d->holdover_running_s);
        cJSON_AddNumberToObject(root, "holdovers", d->holdovers);
        cJSON_AddNumberToObject(root, "last_holdover_end_s", static_cast<double>(d->last_holdover_end_s));
        cJSON_AddNumberToObject(root, "last_holdover_s", d->last_holdover_s);
        cJSON_AddNumberToObject(root, "last_holdover_error_ns", d->last_holdover_error_ns);
        cJSON_AddBoolToObject(root, "last_holdover_stepped", d->last_holdover_stepped);
        cJSON_AddNumberToObject(root, "rms_minute_ns", d->rms_minute_ns);
        cJSON_AddNumberToObject(root, "rms_minute_samples", d->rms_minute_samples);
        cJSON_AddNumberToObject(root, "rms_hour_ns", d->rms_hour_ns);
        cJSON_AddNumberToObject(root, "rms_hour_samples", d->rms_hour_samples);
        cJSON_AddNumberToObject(root, "rms_age_s", d->rms_age_s == UINT32_MAX ? -1.0 : static_cast<double>(d->rms_age_s));
        cJSON_AddNumberToObject(root, "stability_samples", d->stability_samples);
        cJSON_AddNumberToObject(root, "stability_source", d->stability_source);
        cJSON *stability = cJSON_AddArrayToObject(root, "stability");
        for (size_t i = 0; i < d->stability_count && i < WEB_DIAG_TAUS; ++i)
        {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddNumberToObject(row, "tau", d->tau_s[i]);
            cJSON_AddNumberToObject(row, "adev", d->adev[i]);
            cJSON_AddNumberToObject(row, "tdev_ns", d->tdev_ns[i]);
            cJSON_AddItemToArray(stability, row);
        }
        cJSON_AddNumberToObject(root, "isr_latency_us", d->isr_latency_us);
        cJSON_AddItemToObject(root, "isr_bins", cJSON_CreateIntArray(reinterpret_cast<const int *>(d->isr_bins), WEB_DIAG_ISR_BINS));
        cJSON_AddNumberToObject(root, "isr_total", d->isr_total);
        cJSON_AddNumberToObject(root, "isr_max_ns", d->isr_max_ns);
        cJSON_AddBoolToObject(root, "ublox", d->ublox);
        cJSON_AddNumberToObject(root, "qerr_sign", d->qerr_sign);
        cJSON_AddNumberToObject(root, "qerr_samples", d->qerr_samples);
        cJSON_AddNumberToObject(root, "qerr_sd_ns", d->qerr_sd_ns);
        cJSON_AddNumberToObject(root, "qerr_matched", d->qerr_matched);
        cJSON_AddNumberToObject(root, "qerr_unmatched", d->qerr_unmatched);
        cJSON_AddNumberToObject(root, "qerr_invalid", d->qerr_invalid);
        cJSON_AddNumberToObject(root, "tp_messages", d->tp_messages);
        cJSON_AddNumberToObject(root, "tp_last_qerr_ps", d->tp_last_qerr_ps);
        cJSON_AddNumberToObject(root, "tp_last_flags", d->tp_last_flags);
        cJSON_AddNumberToObject(root, "hw_replies", d->hw_replies);
        cJSON_AddNumberToObject(root, "hw_interleaved", d->hw_interleaved);
        cJSON_AddNumberToObject(root, "hw_no_tx_stamp", d->hw_no_tx_stamp);
        cJSON_AddNumberToObject(root, "sw_replies", d->sw_replies);
        cJSON_AddNumberToObject(root, "sw_interleaved", d->sw_interleaved);
        cJSON_AddNumberToObject(root, "shed", d->shed);
        cJSON_AddNumberToObject(root, "lookup_record_hits", d->lookup_record_hits);
        cJSON_AddNumberToObject(root, "lookup_map_hits", d->lookup_map_hits);
        cJSON_AddNumberToObject(root, "lookup_misses", d->lookup_misses);
        cJSON_AddBoolToObject(root, "tx_delay", d->tx_delay);
        cJSON_AddNumberToObject(root, "tx_delay_v4_ns", d->tx_delay_v4_ns);
        cJSON_AddNumberToObject(root, "tx_delay_v4_samples", d->tx_delay_v4_samples);
        cJSON_AddNumberToObject(root, "tx_delay_v6_ns", d->tx_delay_v6_ns);
        cJSON_AddNumberToObject(root, "tx_delay_v6_samples", d->tx_delay_v6_samples);
        cJSON_AddItemToObject(root, "tx_delay_min_ns", cJSON_CreateIntArray(reinterpret_cast<const int *>(d->tx_delay_min_ns), 2));
        cJSON_AddItemToObject(root, "tx_delay_max_ns", cJSON_CreateIntArray(reinterpret_cast<const int *>(d->tx_delay_max_ns), 2));
        cJSON *phase = cJSON_AddArrayToObject(root, "tx_delay_phase");
        for (size_t family = 0; family < 2; ++family)
        {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddItemToObject(row, "ns", cJSON_CreateIntArray(reinterpret_cast<const int *>(d->tx_delay_phase_ns[family]), WEB_DIAG_TX_PHASE_BINS));
            cJSON_AddItemToObject(row, "n", cJSON_CreateIntArray(reinterpret_cast<const int *>(d->tx_delay_phase_samples[family]), WEB_DIAG_TX_PHASE_BINS));
            cJSON_AddItemToArray(phase, row);
        }
        cJSON_AddNumberToObject(root, "uart_overflows", d->uart_overflows);
        cJSON_AddNumberToObject(root, "nmea_sentences", d->nmea_sentences);
        cJSON_AddNumberToObject(root, "nmea_checksum_errors", d->nmea_checksum_errors);
        cJSON_AddItemToObject(root, "rmc_phase_bins", cJSON_CreateIntArray(reinterpret_cast<const int *>(d->rmc_phase_bins), WEB_DIAG_RMC_BINS));
        cJSON_AddNumberToObject(root, "ubx_frames", d->ubx_frames);
        cJSON_AddNumberToObject(root, "ubx_naks", d->ubx_naks);
        cJSON_AddNumberToObject(root, "first_resolved_s", static_cast<double>(d->first_resolved_s));
        cJSON_AddNumberToObject(root, "resolved_losses", d->resolved_losses);
        cJSON_AddNumberToObject(root, "first_valid_utc_s", static_cast<double>(d->first_valid_utc_s));
        cJSON_AddNumberToObject(root, "valid_utc_losses", d->valid_utc_losses);
        cJSON_AddBoolToObject(root, "tp_known", d->tp_known);
        cJSON_AddBoolToObject(root, "tp_enabled", d->tp_enabled);
        cJSON_AddBoolToObject(root, "tp_frequency_mode", d->tp_frequency_mode);
        cJSON_AddBoolToObject(root, "tp_length_mode", d->tp_length_mode);
        cJSON_AddNumberToObject(root, "tp_period", d->tp_period);
        cJSON_AddNumberToObject(root, "tp_period_lock", d->tp_period_lock);
        cJSON_AddNumberToObject(root, "tp_length_us", d->tp_length_us);
        cJSON_AddNumberToObject(root, "tp_length_lock_us", d->tp_length_lock_us);
        cJSON_AddBoolToObject(root, "tp_rising", d->tp_rising);
        cJSON_AddBoolToObject(root, "tp_aligned", d->tp_aligned);
        cJSON_AddNumberToObject(root, "tp_time_grid", d->tp_time_grid);
        cJSON_AddNumberToObject(root, "tp_cable_delay_ns", d->tp_cable_delay_ns);
        cJSON_AddNumberToObject(root, "tp_user_delay_ns", d->tp_user_delay_ns);
        cJSON_AddBoolToObject(root, "rf_known", d->rf_known);
        cJSON_AddNumberToObject(root, "rf_noise_per_ms", d->rf_noise_per_ms);
        cJSON_AddNumberToObject(root, "rf_agc_count", d->rf_agc_count);
        cJSON_AddNumberToObject(root, "rf_jam_indicator", d->rf_jam_indicator);
        cJSON_AddNumberToObject(root, "uptime_s", static_cast<double>(esp_timer_get_time() / 1000000LL));
        delete d;
        crash_dump_info_t crash{};
        crash_dump_get(&crash);
        cJSON *dump = cJSON_AddObjectToObject(root, "crash");
        cJSON_AddBoolToObject(dump, "present", crash.present);
        cJSON_AddStringToObject(dump, "task", crash.task);
        char pc[12];
        snprintf(pc, sizeof(pc), "0x%08lx", static_cast<unsigned long>(crash.pc));
        cJSON_AddStringToObject(dump, "pc", pc);
        cJSON_AddStringToObject(dump, "reason", crash.reason);
        cJSON_AddNumberToObject(dump, "size", crash.size);
        return send_json(req, "200 OK", root);
    }

    // Core dump of the last crash (review E-07): the raw content of the partition (header, ELF core file, checksum),
    // for "idf.py coredump-info --core coredump.bin --core-format raw". Reading the flash stops the cache for a
    // moment per block, so it is only read on request.
    esp_err_t handle_coredump(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, false, token) != gate_t::passed)
            return ESP_OK;
        crash_dump_info_t crash{};
        crash_dump_get(&crash);
        if (!crash.present)
            return send_error(req, "404 Not Found", "No core dump stored");
        static constexpr size_t BLOCK = 2048;
        uint8_t *block = static_cast<uint8_t *>(malloc(BLOCK));
        if (block == nullptr)
            return send_error(req, "503 Service Unavailable", "Out of memory");
        set_security_headers(req, "no-store");
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
        esp_err_t err = ESP_OK;
        for (size_t offset = 0; offset < crash.size && err == ESP_OK; offset += BLOCK)
        {
            const size_t length = std::min<size_t>(BLOCK, crash.size - offset);
            if (!crash_dump_read(offset, block, length))
            {
                err = ESP_FAIL;
                break;
            }
            err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(block), static_cast<ssize_t>(length));
        }
        free(block);
        if (err != ESP_OK)
        {
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL; // the connection is closed: the download is visibly incomplete
        }
        return httpd_resp_send_chunk(req, nullptr, 0);
    }

    esp_err_t handle_coredump_erase(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        if (!crash_dump_erase())
            return send_error(req, "500 Internal Server Error", "The core dump could not be erased");
        return send_ok(req, "Core dump erased.");
    }

    esp_err_t handle_info(httpd_req_t *req)
    {
        char hostname[WEB_HOSTNAME_MAX_LEN + 1];
        const web_settings_t &boot = web_settings_boot();
        web_settings_hostname(boot, hostname, sizeof(hostname));
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "hostname", hostname);
        cJSON_AddStringToObject(root, "fqdn", boot.fqdn);
        return send_json(req, "200 OK", root);
    }

    esp_err_t handle_allow_list(httpd_req_t *req)
    {
        char token[WEB_SESSION_TOKEN_HEX_LEN + 1];
        if (check_session(req, true, token) != gate_t::passed)
            return ESP_OK;
        cJSON *root = read_json(req);
        if (root == nullptr)
            return ESP_OK;
        cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "entries");
        web_allow_entry_t entries[WEB_ALLOW_LIST_MAX]{};
        size_t count = 0;
        char err[96] = "";
        if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > static_cast<int>(WEB_ALLOW_LIST_MAX))
            snprintf(err, sizeof(err), "At most %u entries", static_cast<unsigned>(WEB_ALLOW_LIST_MAX));
        else
        {
            cJSON *item = nullptr;
            cJSON_ArrayForEach(item, list)
            {
                if (!cJSON_IsString(item) || strlen(item->valuestring) > 50 || !web_auth_parse_cidr(item->valuestring, entries[count]))
                {
                    snprintf(err, sizeof(err), "Entry %u is not an address or CIDR network", static_cast<unsigned>(count + 1));
                    break;
                }
                ++count;
            }
        }
        cJSON_Delete(root);
        if (err[0] != '\0')
            return send_error(req, "400 Bad Request", err);
        struct sockaddr_storage peer{};
        peer_address(req, peer, false);
        const esp_err_t result = web_auth_set_allow_list(entries, count, peer);
        if (result == ESP_ERR_INVALID_STATE)
            return send_error(req, "409 Conflict", "Refused: your own address would not be allowed any more");
        if (result != ESP_OK)
            return send_error(req, "500 Internal Server Error", "Allow-list could not be stored");
        return send_ok(req, count == 0 ? "Allow-list cleared: every source address may connect." : "Allow-list stored and active.");
    }

    bool register_handlers(httpd_handle_t server)
    {
        struct route_t
        {
            const char *uri;
            httpd_method_t method;
            esp_err_t (*handler)(httpd_req_t *);
        };
        static const route_t routes[] = {
            {"/", HTTP_GET, handle_index},
            {"/app.js", HTTP_GET, handle_js},
            {"/app.css", HTTP_GET, handle_css},
            {"/i18n.js", HTTP_GET, handle_i18n},
            {"/favicon.ico", HTTP_GET, handle_favicon},
            {"/api/login", HTTP_POST, handle_login},
            {"/api/logout", HTTP_POST, handle_logout},
            {"/api/state", HTTP_GET, handle_state},
            {"/api/apply", HTTP_POST, handle_apply},
            {"/api/save", HTTP_POST, handle_save},
            {"/api/network/confirm", HTTP_POST, handle_confirm},
            {"/api/factory-reset", HTTP_POST, handle_factory_reset},
            {"/api/password", HTTP_POST, handle_password},
            {"/api/allow-list", HTTP_POST, handle_allow_list},
            {"/api/reboot", HTTP_POST, handle_reboot},
            {"/api/ote", HTTP_POST, handle_ote},
            {"/api/ote/once", HTTP_POST, handle_ote_once},
            {"/api/info", HTTP_GET, handle_info},
            {"/api/diag", HTTP_GET, handle_diag},
            {"/api/coredump", HTTP_GET, handle_coredump},
            {"/api/coredump/erase", HTTP_POST, handle_coredump_erase},
        };
        for (const route_t &route : routes)
        {
            httpd_uri_t uri{};
            uri.uri = route.uri;
            uri.method = route.method;
            uri.handler = route.handler;
            if (httpd_register_uri_handler(server, &uri) != ESP_OK)
                return false;
        }
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_not_found);
        return true;
    }

    void web_admin_start_task(void *)
    {
        const web_settings_t &boot = web_settings_boot();
        web_auth_init();

        if (!web_tls_load_or_create(boot.fqdn, s_identity))
        {
            ESP_LOGE(TAG, "Web admin: no TLS identity, the settings page is not available");
            vTaskDelete(nullptr);
            return;
        }
        // Always printed: compare it with the fingerprint shown by the browser before trusting the certificate
        ESP_LOGI(TAG, "Web admin certificate SHA-256 fingerprint: %s", s_identity.fingerprint);

        s_tls_config = esp_tls_cfg_server_t{};
        s_tls_config.servercert_buf = s_identity.certificate_der;
        s_tls_config.servercert_bytes = s_identity.certificate_length;
        s_tls_config.serverkey_buf = s_identity.key_der;
        s_tls_config.serverkey_bytes = s_identity.key_length;
        s_tls_config.tls_handshake_timeout_ms = HANDSHAKE_TIMEOUT_MS;
        s_tls_config.tls_version = ESP_TLS_VER_ANY; // TLS 1.2 and 1.3 (mbedTLS 4 has nothing older)
        s_tls_config.ciphersuites_list = s_ciphersuites;

        s_pending = new (std::nothrow) network_pending_t{};
        esp_timer_create_args_t timer_args{};
        timer_args.callback = confirm_timer_callback;
        timer_args.name = "web_net_confirm";
        if (s_pending == nullptr || esp_timer_create(&timer_args, &s_confirm_timer) != ESP_OK)
        {
            ESP_LOGE(TAG, "Web admin: out of memory");
            vTaskDelete(nullptr);
            return;
        }

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = HTTPS_PORT;
        config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1; // loopback only (httpd control socket)
        config.task_priority = TASK_PRIORITY;
        config.core_id = TASK_CORE;
        config.stack_size = TASK_STACK;
        config.max_open_sockets = MAX_SOCKETS;
        config.lru_purge_enable = true;
        config.backlog_conn = 3;
        config.recv_wait_timeout = IO_TIMEOUT_S;
        config.send_wait_timeout = IO_TIMEOUT_S;
        config.max_uri_handlers = 22; // 21 routes in use
        config.max_resp_headers = 12;
        config.open_fn = transport_open;
        config.uri_match_fn = nullptr; // exact match

        const esp_err_t err = httpd_start(&s_server, &config);
        if (err != ESP_OK || !register_handlers(s_server))
        {
            ESP_LOGE(TAG, "Web admin: HTTPS server could not be started (%s)", esp_err_to_name(err));
            if (s_server != nullptr)
                httpd_stop(s_server);
            s_server = nullptr;
            vTaskDelete(nullptr);
            return;
        }

        ESP_LOGI(TAG, "Web admin: https://%s/ (TCP %u, IPv4 and IPv6)", boot.fqdn, static_cast<unsigned int>(HTTPS_PORT));
        s_running.store(true, std::memory_order_release);

        bool network_confirm = false;
        if (web_settings_boot_was_oneshot(&network_confirm) && network_confirm)
        {
            web_settings_t saved{};
            if (!web_settings_get_saved(saved))
                web_settings_defaults(saved);
            start_network_confirmation(saved, boot, true);
            ESP_LOGW(TAG, "Web admin: this start uses changed network settings; confirm them on the page within 120 s or the device restarts with the saved settings");
        }
        vTaskDelete(nullptr);
    }
} // namespace

bool web_admin_running()
{
    return s_running.load(std::memory_order_acquire);
}

void web_admin_start()
{
#if !WEB_ADMIN_ENABLED
    // WEB_ADMIN_ENABLED 0: no server, no TLS key, no password hash (nothing listens on TCP 443)
    return;
#endif
    if (xTaskCreatePinnedToCore(web_admin_start_task, "web_admin_init", START_TASK_STACK, nullptr, TASK_PRIORITY, nullptr, TASK_CORE) != pdPASS)
        ESP_LOGE(TAG, "Web admin: start task could not be created");
}
