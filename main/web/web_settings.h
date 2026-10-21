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
static constexpr size_t WEB_NTP_KEY_SIZE = 33;      // 32 characters + NUL
static constexpr size_t WEB_OTE_NAME_SIZE = 33;     // OTE device name (mDNS host name), 32 characters + NUL
static constexpr size_t WEB_OTE_PASSWORD_SIZE = 65; // 64 characters + NUL

enum class web_led_type_t : uint8_t
{
    none = 0,
    ky016 = 1,
    ws2812 = 2,
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

    // NTP symmetric-key authentication
    bool ntp_auth_enabled;
    bool ntp_key_override;   // false = the keys of ESP32TimeServerSettingsSecrets.h are used
    uint32_t ntp_key_id;
    char ntp_key[WEB_NTP_KEY_SIZE]; // secret

    // Over The Ethernet updates (ArduinoOTA)
    bool ote_enabled;
    char ote_device_name[WEB_OTE_NAME_SIZE]; // announced by mDNS as <name>.local
    uint16_t ote_port;
    char ote_password[WEB_OTE_PASSWORD_SIZE]; // secret
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

// Constant time comparison of two byte ranges of the same length
bool web_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t length);

// Implemented in main.cpp: applies the live settings (time zone, LCD text options, LED brightness, NTP
// authentication) after web_settings_set_running()
void app_apply_live_settings(const web_settings_t &settings);
// Implemented in main.cpp: controlled restart (MQTT final report etc.)
void app_controlled_restart(const char *reason);
