// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//
// Run-time settings of the web admin page.
//
// Order of precedence at boot: compiled defaults (ESP32TimeServerSettings.h / ESP32TimeServerSettingsSecrets.h),
// then the settings saved in NVS (namespace "webcfg", key "saved"), then a one-shot set written by "Apply" for
// settings that need a restart (key "oneshot", erased at the boot that uses it, so the next boot returns to the
// saved state).

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

static constexpr size_t WEB_FQDN_SIZE = 254;        // 253 characters + NUL
static constexpr size_t WEB_HOSTNAME_MAX_LEN = 32;  // first label of the FQDN (DHCP host name, esp_netif limit)
static constexpr size_t WEB_IPV4_TEXT_SIZE = 16;
static constexpr size_t WEB_IPV6_TEXT_SIZE = 46;
static constexpr size_t WEB_DNS_SERVER_COUNT = 3;   // lwIP default DNS_MAX_SERVERS
static constexpr size_t WEB_TZ_SIZE = 64;
static constexpr size_t WEB_LCD_TITLE_SIZE = 18;    // 17 characters + NUL (the last 3 LCD columns are status indicators)
static constexpr size_t WEB_MQTT_URI_SIZE = 128;
static constexpr size_t WEB_MQTT_CREDENTIAL_SIZE = 65;
static constexpr size_t WEB_MQTT_TOPIC_SIZE = 101;  // 100 characters + NUL (topics <topic>/report and <topic>/status)
static constexpr uint16_t WEB_PPS_LOST_SERVE_MAX_MIN = 1440; // serving time after the PPS loss: up to 24 h (0 = no limit)
static constexpr uint16_t WEB_PPS_HOLDOVER_MAX_S = 600;       // PPS holdover: 0 to 10 min (below the 15 min GNSS sync stale limit)
static constexpr uint16_t WEB_PPS_DISPERSION_MAX_PPM = 10000; // root dispersion growth while coasting: up to 10 ms per second
static constexpr uint16_t WEB_MQTT_TF_CLIENT_LIMIT_MIN = 50;   // MQTT_CLIENT_LIMIT (report queue in RAM)
static constexpr uint16_t WEB_MQTT_TF_CLIENT_LIMIT_MAX = 1000; // report buffer about 77 kB (no PSRAM)
static constexpr size_t WEB_MAC_TEXT_SIZE = 18;     // "xx:xx:xx:xx:xx:xx" + NUL
static constexpr size_t WEB_NTP_KEY_SIZE = 33;      // 32 characters + NUL
static constexpr size_t WEB_NTP_MAX_KEYS = 8;       // NTP_AUTH_MAX_KEYS
static constexpr size_t WEB_OTE_NAME_SIZE = 33;     // OTE device name (mDNS host name), 32 characters + NUL
static constexpr size_t WEB_OTE_PASSWORD_SIZE = 65; // 64 characters + NUL

enum class web_led_type_t : uint8_t
{
    none = 0,
    ky016 = 1,
    ws2812 = 2,
};

// Symmetric NTP key (key ID and 32 letters or digits)
struct web_ntp_key_t
{
    uint32_t id;
    char key[WEB_NTP_KEY_SIZE];
};

// States shown by the RGB LED; each has a colour in the settings (LED_startup ... LED_button_push)
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

    // MQTT
    bool mqtt_enabled;
    char mqtt_uri[WEB_MQTT_URI_SIZE];
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

    // TF card (MQTT report queue for QoS 1 and 2), SDMMC slot 1 in 4-bit mode
    int8_t tf_cmd_pin;
    int8_t tf_clk_pin;
    int8_t tf_d0_pin;
    int8_t tf_d1_pin;
    int8_t tf_d2_pin;
    int8_t tf_d3_pin;

    // NTP symmetric-key authentication
    bool ntp_auth_enabled;
    bool ntp_key_override;   // false = the keys of ESP32TimeServerSettingsSecrets.h are used
    uint8_t ntp_key_count;   // keys of the web settings (ntp_key_override)
    web_ntp_key_t ntp_keys[WEB_NTP_MAX_KEYS]; // secret (shown on the page: the clients need them)

    // Over The Ethernet updates (firmware upload over the web admin server, see web_ote.h)
    bool ote_enabled;
    char ote_device_name[WEB_OTE_NAME_SIZE]; // mDNS host name: announced as <name>.local (also while OTE is off)
    // PPS holdover in s after the PPS counts as lost (0 to WEB_PPS_HOLDOVER_MAX_S, takes effect at once). Stored in the
    // place of the former ArduinoOTA port (same layout as blob version 5, which is read with the compiled default).
    uint16_t pps_holdover_s;
    char ote_password[WEB_OTE_PASSWORD_SIZE]; // secret: "Authorization: Bearer" of scripted uploads

    // PPS lost for longer than the holdover (takes effect at once; added in version 6, after the fields of version 5)
    uint8_t pps_lost_stratum; // stratum of the answers: 2 to 15 (while coasting is possible), 16 = not synchronised
    bool pps_lost_serve;      // false: NTP requests are not answered
    uint16_t pps_lost_serve_min; // with pps_lost_serve: answers for this long after the PPS was lost (0 = no limit)

    // Growth of the advertised root dispersion while coasting after the PPS loss (holdover and stratum 2 to 15), in ppm
    // of the time since the last pulse = us per second (1 to WEB_PPS_DISPERSION_MAX_PPM, takes effect at once; added
    // in version 7)
    uint16_t pps_dispersion_ppm;
};

// Compiled defaults
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

// Erases the saved settings, the one-shot set and the web admin password and allow-list.
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
// MAC address as "xx:xx:xx:xx:xx:xx", "xx-xx-xx-xx-xx-xx", "xxxxxxxxxxxx" or "xxxx.xxxx.xxxx" (any case) to lower
// case with ':'; false if not valid
bool web_normalise_mac(const char *text, char (&out)[WEB_MAC_TEXT_SIZE]);
// Hardware (eFuse) MAC of the chip; with one universal MAC address it is the Ethernet MAC
bool web_hardware_mac(char (&out)[WEB_MAC_TEXT_SIZE]);
// Size of the MQTT report buffer for a client limit (tools/json_message_calculator.xlsx: 1166 bytes plus 76 bytes per
// client; 50 clients = 4966 = MQTT_REPORT_SIZE, 500 clients = 39166)
constexpr size_t web_mqtt_report_size(size_t client_limit) { return 1166 + 76 * client_limit; }

// Constant time comparison of two byte ranges of the same length
bool web_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t length);

// Implemented in main.cpp: applies the live settings (time zone, LCD text options, LED brightness, NTP
// authentication) after web_settings_set_running()
void app_apply_live_settings(const web_settings_t &settings);
// Implemented in main.cpp: controlled restart (MQTT final report etc.)
void app_controlled_restart(const char *reason);
