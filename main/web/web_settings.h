// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Run-time settings of the web admin page.
//
// All settings are made on the web page; there is no settings file. Order of precedence at boot: the defaults
// (web_settings_defaults), then the settings saved in NVS (namespace "webcfg", key "saved"), then a one-shot set written by "Apply" for
// settings that need a restart (key "oneshot", erased at the boot that uses it, so the next boot returns to the
// saved state).

#pragma once

#include <cstddef>
#include <cstdint>

#include "auth/auth_config.h"
#include "esp_err.h"
#include "soc/soc_caps.h"

// Highest GPIO number of the chip (ESP32-P4: 54, ESP32-S31: 61); web_is_reserved_gpio() also refuses the numbers the
// chip does not have
static constexpr int WEB_GPIO_MAX = SOC_GPIO_PIN_COUNT - 1;

static constexpr size_t WEB_FQDN_SIZE = 254;        // 253 characters + NUL
static constexpr size_t WEB_HOSTNAME_MAX_LEN = 32;  // first label of the FQDN (DHCP host name, esp_netif limit)
static constexpr size_t WEB_IPV4_TEXT_SIZE = 16;
static constexpr size_t WEB_IPV6_TEXT_SIZE = 46;
static constexpr size_t WEB_DNS_SERVER_COUNT = 3;   // lwIP default DNS_MAX_SERVERS
static constexpr size_t WEB_TZ_SIZE = 64;
static constexpr size_t WEB_LCD_TITLE_SIZE = 18;    // 17 characters + NUL (the last 3 LCD columns are status indicators)
static constexpr size_t WEB_MQTT_URI_SIZE = 128;   // former broker URI (blob versions up to 12, see mqtt_uri_unused)
static constexpr size_t WEB_MQTT_CA_SIZE = 4097;   // CA certificate of the broker (PEM), 4096 characters + NUL
static constexpr size_t WEB_MQTT_CREDENTIAL_SIZE = 65;
static constexpr size_t WEB_MQTT_TOPIC_SIZE = 101;  // 100 characters + NUL (topics <topic>/report and <topic>/status)
static constexpr uint16_t WEB_PPS_LOST_SERVE_MAX_MIN = 1440; // serving time after the PPS loss: up to 24 h (0 = no limit)
static constexpr uint16_t WEB_PPS_HOLDOVER_MAX_S = 600;       // PPS holdover: 0 to 10 min (below the 15 min GNSS sync stale limit)
static constexpr uint16_t WEB_PPS_DISPERSION_MAX_PPM = 10000; // root dispersion growth while coasting: up to 10 ms per second
static constexpr uint16_t WEB_NTP_RATE_LIMIT_MAX = 10000;     // NTP requests per second and client (0 = no limit)
static constexpr uint16_t WEB_GNSS_MIN_FIX_AGE_MAX_S = 3600;  // minimum GNSS fix age before stratum 1: up to 1 h (0 = off)
static constexpr uint8_t WEB_GNSS_MIN_SATELLITES_MAX = 50;    // minimum satellites in use before stratum 1 (0 = off)
static constexpr uint16_t WEB_MQTT_TF_CLIENT_LIMIT_MIN = 50;   // MQTT_CLIENT_LIMIT (report queue in RAM)
static constexpr uint16_t WEB_MQTT_TF_CLIENT_LIMIT_MAX = 1000; // report buffer about 77 kB (no PSRAM)
static constexpr uint32_t WEB_BUTTON_HOLD_S = 10;              // uptime / restart button: hold time for a restart or reset
static constexpr size_t WEB_MAC_TEXT_SIZE = 18;     // "xx:xx:xx:xx:xx:xx" + NUL
// PTP master (blob version 14): limits of the settings
static constexpr uint8_t WEB_PTP_DOMAIN_MAX = 127;              // default profile: domains 0 to 127
static constexpr int8_t WEB_PTP_LOG_SYNC_MIN = -3, WEB_PTP_LOG_SYNC_MAX = 4;
static constexpr int8_t WEB_PTP_LOG_ANNOUNCE_MIN = 0, WEB_PTP_LOG_ANNOUNCE_MAX = 4;
static constexpr int8_t WEB_PTP_LOG_DELAY_REQ_MIN = -3, WEB_PTP_LOG_DELAY_REQ_MAX = 6;
static constexpr uint16_t WEB_PTP_FOLLOWER_TIMEOUT_MIN_S = 5, WEB_PTP_FOLLOWER_TIMEOUT_MAX_S = 3600;
static constexpr uint16_t WEB_PTP_DELAY_REQ_RATE_MAX = 1000;   // per follower and second (0 = no limit)
static constexpr int16_t WEB_PTP_UTC_OFFSET_MAX_S = 100;       // TAI - UTC
static constexpr int32_t WEB_PTP_LATENCY_MAX_NS = 100000;      // ingress / egress latency: -100 to 100 us
static constexpr size_t WEB_NTP_KEY_SIZE = 33;      // 32 characters + NUL
static constexpr size_t WEB_NTP_MAX_KEYS = 8;       // NTP_AUTH_MAX_KEYS
static constexpr size_t WEB_PTP_MAX_KEYS = 4;       // PTP_AUTH_MAX_KEYS
// Sizes of the former OTE device name and OTE password fields (unused, they keep the layout of the saved settings)
static constexpr size_t WEB_OTE_NAME_SIZE = 33;
static constexpr size_t WEB_OTE_PASSWORD_SIZE = 65;

enum class web_led_type_t : uint8_t
{
    none = 0,
    ky016 = 1,
    ws2812 = 2,
};

// Address families in use (needs a restart)
enum class web_ip_mode_t : uint8_t
{
    dual = 0, // IPv4 and IPv6
    ipv4 = 1, // IPv4 only (no IPv6 address at all, no NTP over IPv6)
    ipv6 = 2, // IPv6 only (no DHCPv4, no IPv4 address, no NTP over IPv4)
};

// Address shown in the bottom line of the LCD
enum class web_lcd_address_t : uint8_t
{
    both = 0,   // the IPv4 and the IPv6 address in turn (only the one of the family in use with IPv4 or IPv6 only)
    ipv4 = 1,   // the IPv4 address (the IPv6 address while there is none)
    ipv6 = 2,   // the IPv6 address (the IPv4 address while there is none)
    status = 3, // the state of both families: "IPv4: OK/N/A  IPv6: GUA/ULA/LLA/N/A"
    none = 4,   // nothing (privacy)
};

// Symmetric NTP key (key ID and 32 letters or digits)
struct web_ntp_key_t
{
    uint32_t id;
    char key[WEB_NTP_KEY_SIZE];
};

// States shown by the RGB LED; each has a colour in the settings
enum class web_led_state_t : uint8_t
{
    startup = 0,
    normal,
    sync,
    warning,
    alert,
    critical,
    button,
};
static constexpr size_t WEB_LED_STATE_COUNT = 7;

struct web_settings_t
{
    // Network
    char fqdn[WEB_FQDN_SIZE];
    bool ipv4_dhcp;
    char ipv4_address[WEB_IPV4_TEXT_SIZE];
    uint8_t ipv4_prefix;
    char ipv4_gateway[WEB_IPV4_TEXT_SIZE];
    bool ipv6_slaac;
    char ipv6_address[WEB_IPV6_TEXT_SIZE]; // additional static address, "" = none
    uint8_t ipv6_prefix;
    char ipv6_gateway[WEB_IPV6_TEXT_SIZE]; // "" = use the routers learned from router advertisements
    char dns[WEB_DNS_SERVER_COUNT][WEB_IPV6_TEXT_SIZE];
    char mac_address[WEB_MAC_TEXT_SIZE]; // "" = the hardware MAC of the chip, else lower case "xx:xx:xx:xx:xx:xx"

    // GNSS and PPS
    bool gnss_ublox; // true = u-blox UBX protocol (baud rate found automatically), false = NMEA standard protocol
    int8_t gnss_tx_pin;
    int8_t gnss_rx_pin;
    int8_t gnss_pps_pin;
    uint32_t gnss_baud;
    int8_t ptp_loopback_pin; // -1 = off

    // LCD
    bool lcd_enabled;
    char lcd_title[WEB_LCD_TITLE_SIZE];
    bool lcd_24_hour;
    bool lcd_pad_hour;
    bool lcd_show_time_zone;
    char time_zone[WEB_TZ_SIZE];

    // RGB LED
    web_led_type_t led_type;
    uint8_t led_brightness_percent;
    int8_t led_red_pin;
    int8_t led_green_pin;
    int8_t led_blue_pin;
    int8_t led_ws2812_pin;
    uint32_t led_rgb[WEB_LED_STATE_COUNT]; // colour per state, 0xRRGGBB

    // MQTT (takes effect at once with Apply: the client reconnects; the broker, its port, TLS and CA certificate are
    // appended at the end, version 13)
    bool mqtt_enabled;
    // Former broker URI (mqtt:// or mqtts://, versions up to 12): read into mqtt_broker, mqtt_port and mqtt_tls, then
    // always zero; only keeps the layout of the saved settings
    char mqtt_uri_unused[WEB_MQTT_URI_SIZE];
    char mqtt_username[WEB_MQTT_CREDENTIAL_SIZE];
    char mqtt_password[WEB_MQTT_CREDENTIAL_SIZE]; // secret
    char mqtt_topic[WEB_MQTT_TOPIC_SIZE];
    uint8_t mqtt_qos;                // 0, 1 or 2
    bool mqtt_retain;                // the broker keeps the latest report
    uint32_t mqtt_report_period_s;
    bool mqtt_client_reporting;      // requests per client address
    bool mqtt_memory_reporting;
    bool mqtt_historical_reporting;  // times of the last synchronisation and faults
    bool mqtt_distinguish_auth;      // separate counts for authenticated and unauthenticated requests
    uint16_t mqtt_tf_client_limit;   // client addresses per report while the reports are queued on the TF card

    // TF card (MQTT report queue for QoS 1 and 2), SDMMC slot 1 in 4-bit mode (the pins need a restart)
    int8_t tf_cmd_pin;
    int8_t tf_clk_pin;
    int8_t tf_d0_pin;
    int8_t tf_d1_pin;
    int8_t tf_d2_pin;
    int8_t tf_d3_pin;

    // NTP symmetric-key authentication
    bool ntp_auth_enabled;
    // Always true since the settings files were removed. false in a set of an older firmware: the keys of the former
    // ESP32TimeServerSettingsSecrets.h were used, which no longer exist (such a set is read with no keys)
    bool ntp_key_override;
    uint8_t ntp_key_count;   // keys of the web settings
    web_ntp_key_t ntp_keys[WEB_NTP_MAX_KEYS]; // secret (shown on the page: the clients need them)

    // Over The Ethernet updates (firmware upload over the web admin server, see web_ote.h)
    bool ote_enabled;
    // Former OTE device name (mDNS host name; mDNS now uses the host name of the FQDN). Always zero: only keeps the
    // layout of the saved settings (blob versions 5 to 8).
    char ote_name_unused[WEB_OTE_NAME_SIZE];
    // PPS holdover in s after the PPS counts as lost (0 to WEB_PPS_HOLDOVER_MAX_S, takes effect at once). Stored in the
    // place of the former ArduinoOTA port (same layout as blob version 5, which is read with the default).
    uint16_t pps_holdover_s;
    // Former OTE password (scripted uploads now use the web admin password). Always zero, like ote_name_unused; a
    // password still stored there is erased from NVS at boot.
    char ote_password_unused[WEB_OTE_PASSWORD_SIZE];

    // PPS lost for longer than the holdover (takes effect at once; added in version 6, after the fields of version 5)
    uint8_t pps_lost_stratum; // stratum of the answers: 2 to 15 (while coasting is possible), 16 = not synchronised
    bool pps_lost_serve;      // false: NTP requests are not answered
    uint16_t pps_lost_serve_min; // with pps_lost_serve: answers for this long after the PPS was lost (0 = no limit)

    // Growth of the advertised root dispersion while coasting after the PPS loss (holdover and stratum 2 to 15), in ppm
    // of the time since the last pulse = us per second (1 to WEB_PPS_DISPERSION_MAX_PPM, takes effect at once; added
    // in version 7)
    uint16_t pps_dispersion_ppm;

    // NTP requests per second and client IP address (0 = no limit, at most WEB_NTP_RATE_LIMIT_MAX); requests over the
    // limit are dropped without an answer (takes effect at once; added in version 8)
    uint16_t ntp_client_rate_limit;

    // Minimum GNSS fix age in s before stratum 1 after a start (0 = off, at most WEB_GNSS_MIN_FIX_AGE_MAX_S; takes
    // effect at once until stratum 1 was reached; added in version 9)
    uint16_t gnss_min_fix_age_s;

    // Minimum satellites in use before stratum 1 (0 = off, at most WEB_GNSS_MIN_SATELLITES_MAX; NMEA: from GGA, which
    // is then required; takes effect at once; added in version 10)
    uint8_t gnss_min_satellites;

    // Address families in use (needs a restart) and the address in the bottom line of the LCD (takes effect at once);
    // added in version 11, they replace the former compiled setting PreferIPvX
    web_ip_mode_t ip_mode;
    web_lcd_address_t lcd_address;

    // Uptime / restart button to GND, internal pull-up (needs a restart; added in version 12): a press shows the
    // uptime on the LCD; held for WEB_BUTTON_HOLD_S it restarts the device (GNSS data erased), held while the device
    // starts it resets the settings and the password to their defaults
    bool button_enabled;
    int8_t button_pin;

    // MQTT broker (added in version 13, in the place of the URI): an IPv4 or IPv6 address or a DNS name (no .local,
    // no mDNS); port 0 = 8883 with TLS, 1883 without; with TLS the broker certificate is checked against the CA
    // certificate (PEM) and the broker name or IP address (SAN)
    char mqtt_broker[WEB_FQDN_SIZE];
    uint16_t mqtt_port;
    bool mqtt_tls;
    char mqtt_ca_pem[WEB_MQTT_CA_SIZE];

    // PTP master (added in version 14; everything takes effect at once). Transports: Ethernet (EtherType 0x88F7),
    // UDP/IPv4 and UDP/IPv6 multicast; one domain, clock and settings for all of them
    bool ptp_l2_enabled;
    bool ptp_udp4_enabled;
    bool ptp_udp6_enabled;
    uint8_t ptp_domain;                  // 0 to WEB_PTP_DOMAIN_MAX
    uint8_t ptp_priority1;               // 0 to 255, lower wins
    uint8_t ptp_priority2;
    int8_t ptp_log_sync_interval;        // 2^n s
    int8_t ptp_log_announce_interval;
    int8_t ptp_log_min_delay_req_interval;
    bool ptp_master_selection;           // stand aside for a better master; false = always master
    uint8_t ptp_clock_accuracy;          // clockAccuracy of the Announce: 0x20 to 0x31 or 0xFE (unknown)
    uint16_t ptp_follower_timeout_s;     // a follower without a Delay_Req for this long is gone
    uint16_t ptp_delay_req_rate_limit;   // Delay_Req per follower and second, 0 = no limit
    int16_t ptp_utc_offset_s;            // TAI - UTC while the GNSS receiver does not tell it (NMEA)
    int32_t ptp_ingress_latency_ns;      // subtracted from the receive time stamps
    int32_t ptp_egress_latency_ns;       // added to the transmit time stamps

    // NTP authentication required (added in version 15; takes effect at once): only requests with a valid MAC are
    // answered, unsigned NTPv3 and NTPv4 requests are dropped. false = mixed (signed and unsigned are answered).
    // Needs ntp_auth_enabled and at least one key.
    bool ntp_auth_required;

    // PTP authentication (added in version 16; takes effect at once): AUTHENTICATION TLV with HMAC-SHA256-128 on every
    // transport (custom/ptp_auth.h, linuxptp sa_file type SHA256-128). On: all sent messages are signed with the active
    // key, signed Delay_Req / Announce must verify; required: unsigned ones are dropped as well. mutable: the
    // correctionField is not covered by the ICV (linuxptp allow_mutable 1). Keys like the NTP keys: ID 1 to 65535 and
    // 32 letters or digits (secret, shown on the page: the followers need them).
    bool ptp_auth_enabled;
    bool ptp_auth_required;
    bool ptp_auth_mutable;
    uint8_t ptp_auth_spp;              // securityParameterPointer 0 to 255
    uint16_t ptp_auth_active_key_id;   // signs the sent messages, one of ptp_keys
    uint8_t ptp_key_count;
    web_ntp_key_t ptp_keys[WEB_PTP_MAX_KEYS];

    // Logins of the web page (added in version 17; take effect at once): the RADIUS servers and the attributes of the
    // requests, the login methods and the TOTP second factor (auth/auth_config.h). The shared secrets are secret: the
    // page gets "secret_set"; the stored settings and the configuration file carry them.
    radius_settings_t radius;
    web_login_settings_t web_login;
};

// Defaults of all settings (the state after a reset to defaults)
void web_settings_defaults(web_settings_t &out);

// Loads the settings of this boot (call once, after nvs_flash_init and before anything uses them).
// The one-shot set is erased from NVS before it is used.
void web_settings_load_boot();

// Settings in effect since the boot. Settings that need a restart are only read from here.
const web_settings_t &web_settings_boot();

// The boot used the one-shot set; *network_confirm_required: it changed the network settings and has to be
// confirmed from the new address (otherwise the web admin restarts the device, which returns to the saved state)
bool web_settings_boot_was_oneshot(bool *network_confirm_required);

// Settings currently in use (live settings may have been changed by "Apply" since the boot). Thread safe.
void web_settings_get_running(web_settings_t &out);
void web_settings_set_running(const web_settings_t &in);

// Settings saved in NVS. Returns false if nothing is saved (or the saved data is unusable).
bool web_settings_get_saved(web_settings_t &out);
esp_err_t web_settings_save(const web_settings_t &in);
esp_err_t web_settings_save_oneshot(const web_settings_t &in, bool network_confirm_required);

// Erases the saved settings, the one-shot set and the web admin password and allow-list (back to the defaults).
// The TLS identity and the calibration namespaces are kept.
esp_err_t web_settings_factory_reset();

// Validation of a complete settings set (err gets a short English message for the page)
bool web_settings_validate(const web_settings_t &s, char *err, size_t err_size);

// Comparison helpers
bool web_settings_network_differs(const web_settings_t &a, const web_settings_t &b);
bool web_settings_restart_fields_differ(const web_settings_t &a, const web_settings_t &b);
bool web_settings_equal(const web_settings_t &a, const web_settings_t &b);

// First label of the FQDN (host name)
void web_settings_hostname(const web_settings_t &s, char *out, size_t out_size);

// Field helpers shared with the network and server code
bool web_parse_ipv4(const char *text, uint32_t *address_be);
bool web_parse_ipv6(const char *text, uint8_t address[16]);
bool web_is_valid_time_zone(const char *tz);
bool web_is_reserved_gpio(int gpio);
// MQTT field checks: a broker name ending in .local (with or without the final dot); a broker that is an IPv4 or
// IPv6 address or a DNS name; a topic of 1 to 100 characters without +, #, control characters, a leading $ or a
// trailing /; PEM text of one or more CERTIFICATE blocks of base64 (the content is parsed with mbedTLS by the web
// server, app_mqtt_check_ca)
bool web_mqtt_is_mdns_name(const char *host);
bool web_mqtt_host_valid(const char *host);
bool web_mqtt_topic_valid(const char *topic);
bool web_pem_certificates_valid(const char *pem);
// PEM text with one private key block (PRIVATE KEY, EC PRIVATE KEY or RSA PRIVATE KEY) of base64, optionally after an
// EC PARAMETERS block (openssl ecparam -genkey); no header lines, so an encrypted key does not pass (web/web_tls.h)
bool web_pem_private_key_valid(const char *pem);
// Former broker URI "mqtt://host[:port]" or "mqtts://host[:port]" (an IPv6 address in brackets) into mqtt_broker,
// mqtt_port (0 without a port) and mqtt_tls; false (broker empty) if it is no such URI or the host does not fit the
// broker field
bool web_mqtt_split_uri(const char *uri, web_settings_t &s);
// Port of the broker connection: the setting, else 8883 with TLS and 1883 without
inline uint16_t web_mqtt_port_in_use(const web_settings_t &s) { return s.mqtt_port != 0 ? s.mqtt_port : (s.mqtt_tls ? 8883 : 1883); }
// MAC address as "xx:xx:xx:xx:xx:xx", "xx-xx-xx-xx-xx-xx", "xxxxxxxxxxxx" or "xxxx.xxxx.xxxx" (any case) to lower
// case with ':'; false if not valid
bool web_normalise_mac(const char *text, char (&out)[WEB_MAC_TEXT_SIZE]);
// Hardware (eFuse) MAC of the chip; with one universal MAC address it is the Ethernet MAC
bool web_hardware_mac(char (&out)[WEB_MAC_TEXT_SIZE]);
// Size of the MQTT report buffer for a client limit (tools/json_message_calculator.xlsx: 1166 bytes plus 76 bytes per
// client, plus 260 bytes for the time quality of the servo since 2026-10-26; 50 clients = 5226 = MQTT_REPORT_SIZE,
// 500 clients = 39426)
// PTP part of a report (4.105): counts and counters, and with client reporting at most WEB_MQTT_PTP_CLIENTS follower
// MACs of 64 bytes each
static constexpr size_t WEB_MQTT_PTP_CLIENTS = 32;
static constexpr size_t WEB_MQTT_PTP_REPORT_BYTES = 400 + 64 * WEB_MQTT_PTP_CLIENTS;
constexpr size_t web_mqtt_report_size(size_t client_limit) { return 1426 + WEB_MQTT_PTP_REPORT_BYTES + 76 * client_limit; }

// Constant time comparison of two byte ranges of the same length
bool web_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t length);

// Implemented in main.cpp: applies the live settings (time zone, LCD text options, LED brightness, NTP
// authentication) after web_settings_set_running()
void app_apply_live_settings(const web_settings_t &settings);
// Implemented in main.cpp: controlled restart (MQTT final report etc.)
void app_controlled_restart(const char *reason);
