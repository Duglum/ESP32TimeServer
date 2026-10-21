// ESP32 Time Server
// Copyright Rob Latour, 2026
// License: MIT
// Website: https://github.com/roblatour/ESP32TimeServer
//

#include "web_settings.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <new>

#include "ESP32TimeServerSettings.h"

extern "C"
{
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
}

namespace
{
    const char *TAG = "web_settings";

    constexpr char NVS_NAMESPACE[] = "webcfg";
    constexpr char NVS_KEY_SAVED[] = "saved";
    constexpr char NVS_KEY_ONESHOT[] = "oneshot";
    constexpr char AUTH_NVS_NAMESPACE[] = "webauth"; // admin password hash and allow-list (see web_auth.cpp)

    constexpr uint32_t BLOB_MAGIC = 0x57534554u; // "WSET"
    // Increase when web_settings_t changes: a saved set of another version is ignored (compiled defaults are used)
    // 2: GNSS protocol and LED colours added; 3: Over The Ethernet update settings added; 4: up to 8 NTP keys
    constexpr uint32_t BLOB_VERSION = 4;

    struct settings_blob_t
    {
        uint32_t magic;
        uint32_t version;
        uint32_t size;
        uint32_t flags; // bit 0: one-shot set changed the network (confirmation required)
        web_settings_t settings;
        uint32_t crc;
    };

    web_settings_t s_boot{};
    web_settings_t s_running{};
    bool s_boot_oneshot = false;
    bool s_boot_network_confirm = false;
    SemaphoreHandle_t s_mutex = nullptr;

    template <size_t N>
    void copy_text(char (&destination)[N], const char *source)
    {
        snprintf(destination, N, "%s", source == nullptr ? "" : source);
    }

    uint32_t blob_crc(const settings_blob_t &blob)
    {
        return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&blob), offsetof(settings_blob_t, crc));
    }

    // Reads a settings blob. Returns true if it exists, matches this firmware and passes the CRC.
    bool read_blob(nvs_handle_t handle, const char *key, settings_blob_t &blob)
    {
        size_t length = sizeof(blob);
        const esp_err_t err = nvs_get_blob(handle, key, &blob, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            return false;
        if (err != ESP_OK || length != sizeof(blob))
        {
            ESP_LOGW(TAG, "Stored web settings '%s' have an unexpected size, ignored", key);
            return false;
        }
        if (blob.magic != BLOB_MAGIC || blob.version != BLOB_VERSION || blob.size != sizeof(web_settings_t))
        {
            ESP_LOGW(TAG, "Stored web settings '%s' belong to another firmware version, ignored", key);
            return false;
        }
        if (blob.crc != blob_crc(blob))
        {
            ESP_LOGW(TAG, "Stored web settings '%s' have a CRC error, ignored", key);
            return false;
        }
        // Text fields are always terminated (the CRC covers them, this only protects against a firmware bug)
        blob.settings.fqdn[WEB_FQDN_SIZE - 1] = '\0';
        return true;
    }

    esp_err_t write_blob(const char *key, const web_settings_t &settings, uint32_t flags)
    {
        settings_blob_t blob{};
        blob.magic = BLOB_MAGIC;
        blob.version = BLOB_VERSION;
        blob.size = sizeof(web_settings_t);
        blob.flags = flags;
        blob.settings = settings;
        blob.crc = blob_crc(blob);

        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK)
            return err;
        err = nvs_set_blob(handle, key, &blob, sizeof(blob));
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
        return err;
    }

    bool parse_subnet_mask(const char *text, uint8_t *prefix)
    {
        uint32_t mask_be = 0;
        if (!web_parse_ipv4(text, &mask_be))
            return false;
        const uint32_t mask = ntohl(mask_be);
        uint8_t bits = 0;
        while (bits < 32 && (mask & (0x80000000u >> bits)) != 0)
            ++bits;
        if (bits == 32 ? mask != 0xFFFFFFFFu : (mask << bits) != 0) // not contiguous
            return false;
        *prefix = bits;
        return true;
    }

    bool is_printable_ascii(const char *text, char lowest, char highest)
    {
        for (const char *c = text; *c != '\0'; ++c)
            if (*c < lowest || *c > highest)
                return false;
        return true;
    }

    // ---- POSIX TZ validation ----
    bool tz_name(const char *&p)
    {
        if (*p == '<')
        {
            ++p;
            int n = 0;
            while (isalnum(static_cast<unsigned char>(*p)) || *p == '+' || *p == '-')
            {
                ++p;
                ++n;
            }
            if (*p != '>' || n < 3)
                return false;
            ++p;
            return true;
        }
        int n = 0;
        while (isalpha(static_cast<unsigned char>(*p)))
        {
            ++p;
            ++n;
        }
        return n >= 3;
    }

    bool tz_number(const char *&p, int min_value, int max_value, int max_digits)
    {
        int value = 0;
        int digits = 0;
        while (isdigit(static_cast<unsigned char>(*p)) && digits < max_digits)
        {
            value = value * 10 + (*p - '0');
            ++p;
            ++digits;
        }
        return digits > 0 && !isdigit(static_cast<unsigned char>(*p)) && value >= min_value && value <= max_value;
    }

    // [+-]hh[:mm[:ss]]
    bool tz_time(const char *&p, int max_hours)
    {
        if (*p == '+' || *p == '-')
            ++p;
        if (!tz_number(p, 0, max_hours, 3))
            return false;
        for (int part = 0; part < 2 && *p == ':'; ++part)
        {
            ++p;
            if (!tz_number(p, 0, 59, 2))
                return false;
        }
        return true;
    }

    // Jn, n or Mm.w.d, optionally followed by /time
    bool tz_rule_date(const char *&p)
    {
        if (*p == 'J')
        {
            ++p;
            if (!tz_number(p, 1, 365, 3))
                return false;
        }
        else if (*p == 'M')
        {
            ++p;
            if (!tz_number(p, 1, 12, 2) || *p++ != '.' || !tz_number(p, 1, 5, 1) || *p++ != '.' || !tz_number(p, 0, 6, 1))
                return false;
        }
        else if (!tz_number(p, 0, 365, 3))
            return false;
        if (*p == '/')
        {
            ++p;
            if (!tz_time(p, 167))
                return false;
        }
        return true;
    }
} // namespace

bool web_parse_ipv4(const char *text, uint32_t *address_be)
{
    struct in_addr address{};
    if (text == nullptr || text[0] == '\0' || strlen(text) >= WEB_IPV4_TEXT_SIZE || inet_pton(AF_INET, text, &address) != 1)
        return false;
    if (address_be != nullptr)
        *address_be = address.s_addr;
    return true;
}

bool web_parse_ipv6(const char *text, uint8_t address[16])
{
    struct in6_addr parsed{};
    if (text == nullptr || text[0] == '\0' || strlen(text) >= WEB_IPV6_TEXT_SIZE || strchr(text, '%') != nullptr ||
        inet_pton(AF_INET6, text, &parsed) != 1)
        return false;
    if (address != nullptr)
        memcpy(address, &parsed, 16);
    return true;
}

bool web_is_valid_time_zone(const char *tz)
{
    if (tz == nullptr || tz[0] == '\0' || strlen(tz) >= WEB_TZ_SIZE || !is_printable_ascii(tz, 0x21, 0x7E))
        return false;
    const char *p = tz;
    if (!tz_name(p) || !tz_time(p, 24))
        return false;
    if (*p == '\0')
        return true;
    if (!tz_name(p))
        return false;
    if (*p != '\0' && *p != ',' && !tz_time(p, 24))
        return false;
    if (*p == '\0')
        return true;
    if (*p++ != ',' || !tz_rule_date(p) || *p++ != ',' || !tz_rule_date(p))
        return false;
    return *p == '\0';
}

bool web_is_reserved_gpio(int gpio)
{
    // Waveshare ESP32-P4-ETH: RMII and SMI of the Ethernet PHY, UART0 (console), strapping pins (34 to 38, partly
    // RMII as well), USB Serial/JTAG, TF card slot and the LCD I2C bus
    static constexpr int reserved[] = {
        28, 29, 30, 31, 34, 35, 49, 50, 51, 52, // Ethernet RMII, MDC, MDIO, PHY reset, REF_CLK
        36, 37, 38,                             // strapping; 37/38 = UART0 console
        24, 25,                                 // USB Serial/JTAG
        39, 40, 41, 42, 43, 44,                 // TF card (MQTT report queue)
        7, 8,                                   // LCD I2C (SCL, SDA)
    };
    if (gpio < 0 || gpio > 54)
        return true;
    for (int r : reserved)
        if (gpio == r)
            return true;
#if UPTIME_RESTART_BUTTON_ENABLED
    if (gpio == upTimeRestartPin)
        return true;
#endif
    return false;
}

bool web_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t length)
{
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i)
        difference = difference | (a[i] ^ b[i]);
    return difference == 0;
}

// RGB value of a colour name of ESP32TimeServerSettings.h (LED_startup ...)
static uint32_t web_led_colour_rgb(RGB_LED_Colour colour)
{
    switch (colour)
    {
    case RGB_LED_Colour::red:
        return 0xFF0000;
    case RGB_LED_Colour::green:
        return 0x00FF00;
    case RGB_LED_Colour::blue:
        return 0x0000FF;
    case RGB_LED_Colour::yellow:
        return 0xFFFF00;
    case RGB_LED_Colour::white:
        return 0xFFFFFF;
    case RGB_LED_Colour::orange:
        return 0xFFA500;
    case RGB_LED_Colour::pink:
        return 0xFFC0CB;
    case RGB_LED_Colour::off:
    default:
        return 0x000000;
    }
}

void web_settings_defaults(web_settings_t &out)
{
    out = web_settings_t{};

    copy_text(out.fqdn, DeviceName);

    uint8_t prefix = 24;
    out.ipv4_dhcp = StaticIPAddress[0] == '\0' || !web_parse_ipv4(StaticIPAddress, nullptr) || !parse_subnet_mask(SubnetMask, &prefix);
    if (!out.ipv4_dhcp)
    {
        copy_text(out.ipv4_address, StaticIPAddress);
        copy_text(out.ipv4_gateway, Gateway);
    }
    out.ipv4_prefix = prefix;

    out.ipv6_slaac = true;
    out.ipv6_prefix = 64;
    copy_text(out.dns[0], PrimaryDNS);
    copy_text(out.dns[1], SecondaryDNS);

#ifdef UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED
    out.gnss_ublox = UBLOX_COMPLIANT_GNSS_RECEIVER_ENABLED != 0;
#else
    out.gnss_ublox = true;
#endif
    out.gnss_tx_pin = static_cast<int8_t>(TXPin);
    out.gnss_rx_pin = static_cast<int8_t>(RXPin);
    out.gnss_pps_pin = static_cast<int8_t>(PPSPin);
    out.gnss_baud = baudRateForUbloxNonCompliantGNSSReceiver;
    out.ptp_loopback_pin = static_cast<int8_t>(PTPPPSLoopbackPin);

    out.lcd_enabled = LIQUID_CRYSTAL_DISPLAY_ENABLED != 0;
    copy_text(out.lcd_title, lcdTopLineTitle);
    out.lcd_24_hour = display24HourFormat;
    out.lcd_pad_hour = displayHoursWithPaddedZero;
    out.lcd_show_time_zone = displayTimeZone;
    copy_text(out.time_zone, timeZoneSpec);

#if RBG_LED_KY_016_MODULE_ENABLED && RBG_LED_WS2812_MODULE_ENABLED
#error "KY-016 and WS2812 RGB LED modules cannot both be enabled at the same time"
#endif
    out.led_type = RBG_LED_KY_016_MODULE_ENABLED ? web_led_type_t::ky016
                   : RBG_LED_WS2812_MODULE_ENABLED ? web_led_type_t::ws2812
                                                   : web_led_type_t::none;
    out.led_brightness_percent = static_cast<uint8_t>(RGB_LED_Brightness_Percent < 0 ? 0 : RGB_LED_Brightness_Percent > 100 ? 100 : RGB_LED_Brightness_Percent);
    out.led_red_pin = static_cast<int8_t>(LEDRedPin);
    out.led_green_pin = static_cast<int8_t>(LEDGreenPin);
    out.led_blue_pin = static_cast<int8_t>(LEDBluePin);
    out.led_ws2812_pin = static_cast<int8_t>(WS2812DataPin);
    out.led_rgb[static_cast<size_t>(web_led_state_t::startup)] = web_led_colour_rgb(LED_startup);
    out.led_rgb[static_cast<size_t>(web_led_state_t::normal)] = web_led_colour_rgb(LED_normal);
    out.led_rgb[static_cast<size_t>(web_led_state_t::sync)] = web_led_colour_rgb(LED_sync);
    out.led_rgb[static_cast<size_t>(web_led_state_t::warning)] = web_led_colour_rgb(LED_warning);
    out.led_rgb[static_cast<size_t>(web_led_state_t::alert)] = web_led_colour_rgb(LED_alert);
    out.led_rgb[static_cast<size_t>(web_led_state_t::critical)] = web_led_colour_rgb(LED_critical);
    out.led_rgb[static_cast<size_t>(web_led_state_t::button)] = web_led_colour_rgb(LED_button_push);

    out.mqtt_enabled = MQTT_ENABLED != 0;
    if (MQTTServerIPAddress[0] != '\0')
    {
        const bool ipv6 = strchr(MQTTServerIPAddress, ':') != nullptr;
        snprintf(out.mqtt_uri, sizeof(out.mqtt_uri), ipv6 ? "mqtt://[%s]:%u" : "mqtt://%s:%u", MQTTServerIPAddress,
                 static_cast<unsigned int>(MQTTPort));
    }
    copy_text(out.mqtt_username, MQTTUsername);
    copy_text(out.mqtt_password, MQTTPassword);

    out.ote_enabled = OTE_UPDATES_ENABLED != 0;
    copy_text(out.ote_device_name, DeviceName);
    out.ote_port = OTEPort;
    copy_text(out.ote_password, OTEPassword);

    out.ntp_auth_enabled = SYMMETRIC_KEY_AUTHENTICATION_ENABLED != 0;
    out.ntp_key_override = false; // the keys of ESP32TimeServerSettingsSecrets.h
    out.ntp_key_count = 0;
}

void web_settings_load_boot()
{
    if (s_mutex == nullptr)
        s_mutex = xSemaphoreCreateMutex();

    web_settings_defaults(s_boot);
    const char *source = "compiled defaults";

#if !WEB_ADMIN_ENABLED
    // Web admin disabled: only the compiled settings. Saved settings stay in NVS untouched; a pending one-shot set
    // (from an Apply just before the firmware change) is discarded.
    nvs_handle_t disabled_handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &disabled_handle) == ESP_OK)
    {
        if (nvs_erase_key(disabled_handle, NVS_KEY_ONESHOT) == ESP_OK)
            nvs_commit(disabled_handle);
        nvs_close(disabled_handle);
    }
    s_running = s_boot;
    ESP_LOGI(TAG, "Settings: compiled settings (web admin disabled, WEB_ADMIN_ENABLED 0)");
    return;
#endif

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        static settings_blob_t blob; // static: about 1 kB, the caller (app_main) has a small stack
        char error[96] = "";
        if (read_blob(handle, NVS_KEY_SAVED, blob))
        {
            if (web_settings_validate(blob.settings, error, sizeof(error)))
            {
                s_boot = blob.settings;
                source = "saved settings (NVS)";
            }
            else
                ESP_LOGW(TAG, "Saved web settings are not valid for this firmware (%s), compiled defaults are used", error);
        }

        // The one-shot set is erased before it is used: if it makes the device crash, the next boot is back on the
        // saved settings
        if (read_blob(handle, NVS_KEY_ONESHOT, blob))
        {
            const esp_err_t erase_err = nvs_erase_key(handle, NVS_KEY_ONESHOT);
            const esp_err_t commit_err = erase_err == ESP_OK ? nvs_commit(handle) : erase_err;
            if (commit_err != ESP_OK)
                ESP_LOGE(TAG, "One-shot settings could not be erased (%s), they are not used", esp_err_to_name(commit_err));
            else if (web_settings_validate(blob.settings, error, sizeof(error)))
            {
                s_boot = blob.settings;
                s_boot_oneshot = true;
                s_boot_network_confirm = (blob.flags & 1u) != 0;
                source = "one-shot settings of the web admin (valid for this boot only)";
            }
            else
                ESP_LOGW(TAG, "One-shot web settings are not valid (%s), ignored", error);
        }
        else
        {
            // remove an unusable one-shot entry, if any
            if (nvs_erase_key(handle, NVS_KEY_ONESHOT) == ESP_OK)
                nvs_commit(handle);
        }
        nvs_close(handle);
    }
    else
        ESP_LOGW(TAG, "NVS namespace %s unavailable (%s), compiled defaults are used", NVS_NAMESPACE, esp_err_to_name(err));

    s_running = s_boot;
    ESP_LOGI(TAG, "Settings: %s", source);
}

const web_settings_t &web_settings_boot()
{
    return s_boot;
}

bool web_settings_boot_was_oneshot(bool *network_confirm_required)
{
    if (network_confirm_required != nullptr)
        *network_confirm_required = s_boot_network_confirm;
    return s_boot_oneshot;
}

void web_settings_get_running(web_settings_t &out)
{
    if (s_mutex != nullptr)
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    out = s_running;
    if (s_mutex != nullptr)
        xSemaphoreGive(s_mutex);
}

void web_settings_set_running(const web_settings_t &in)
{
    if (s_mutex != nullptr)
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_running = in;
    if (s_mutex != nullptr)
        xSemaphoreGive(s_mutex);
}

bool web_settings_get_saved(web_settings_t &out)
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;
    settings_blob_t *blob = new (std::nothrow) settings_blob_t{};
    bool ok = blob != nullptr && read_blob(handle, NVS_KEY_SAVED, *blob);
    if (ok)
        out = blob->settings;
    delete blob;
    nvs_close(handle);
    return ok;
}

esp_err_t web_settings_save(const web_settings_t &in)
{
    return write_blob(NVS_KEY_SAVED, in, 0);
}

esp_err_t web_settings_save_oneshot(const web_settings_t &in, bool network_confirm_required)
{
    return write_blob(NVS_KEY_ONESHOT, in, network_confirm_required ? 1u : 0u);
}

esp_err_t web_settings_factory_reset()
{
    esp_err_t result = ESP_OK;
    for (const char *ns : {NVS_NAMESPACE, AUTH_NVS_NAMESPACE})
    {
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            continue;
        if (err == ESP_OK)
        {
            err = nvs_erase_all(handle);
            if (err == ESP_OK)
                err = nvs_commit(handle);
            nvs_close(handle);
        }
        if (err != ESP_OK)
            result = err;
    }
    return result;
}

void web_settings_hostname(const web_settings_t &s, char *out, size_t out_size)
{
    if (out_size == 0)
        return;
    size_t length = strcspn(s.fqdn, ".");
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, s.fqdn, length);
    out[length] = '\0';
}

bool web_settings_validate(const web_settings_t &s, char *err, size_t err_size)
{
#define FAIL(...)                                \
    do                                           \
    {                                            \
        if (err != nullptr && err_size > 0)      \
            snprintf(err, err_size, __VA_ARGS__); \
        return false;                            \
    } while (0)

    // ---- FQDN: host name label plus optional domain, RFC 1123 ----
    {
        const size_t length = strnlen(s.fqdn, WEB_FQDN_SIZE);
        if (length == 0 || length >= WEB_FQDN_SIZE)
            FAIL("FQDN: 1 to 253 characters");
        if (web_parse_ipv4(s.fqdn, nullptr))
            FAIL("FQDN: must not be an IP address");
        size_t label_length = 0;
        size_t label_index = 0;
        bool all_digits = true;
        char previous = '.';
        for (size_t i = 0; i <= length; ++i)
        {
            const char c = s.fqdn[i];
            if (c == '.' || c == '\0')
            {
                if (label_length == 0 || label_length > 63 || previous == '-')
                    FAIL("FQDN: every label needs 1 to 63 characters and must not end with '-'");
                if (label_index == 0 && label_length > WEB_HOSTNAME_MAX_LEN)
                    FAIL("FQDN: the host name (first label) may have at most %u characters", static_cast<unsigned>(WEB_HOSTNAME_MAX_LEN));
                if (c == '\0' && label_index > 0 && all_digits)
                    FAIL("FQDN: the top-level domain must not be numeric");
                ++label_index;
                label_length = 0;
                all_digits = true;
            }
            else
            {
                if (!isalnum(static_cast<unsigned char>(c)) && c != '-')
                    FAIL("FQDN: only letters, digits, '-' and '.' are allowed");
                if (c == '-' && label_length == 0)
                    FAIL("FQDN: a label must not start with '-'");
                if (!isdigit(static_cast<unsigned char>(c)))
                    all_digits = false;
                ++label_length;
            }
            previous = c;
        }
    }

    // ---- IPv4 ----
    if (!s.ipv4_dhcp)
    {
        uint32_t address_be = 0;
        uint32_t gateway_be = 0;
        if (!web_parse_ipv4(s.ipv4_address, &address_be))
            FAIL("IPv4 address is not valid");
        if (s.ipv4_prefix < 8 || s.ipv4_prefix > 30)
            FAIL("IPv4 prefix length: 8 to 30");
        const uint32_t address = ntohl(address_be);
        const uint32_t mask = 0xFFFFFFFFu << (32 - s.ipv4_prefix);
        const uint8_t first = static_cast<uint8_t>(address >> 24);
        if (first == 0 || first == 127 || first >= 224 || (first == 169 && static_cast<uint8_t>(address >> 16) == 254))
            FAIL("IPv4 address: not a usable unicast address");
        if ((address & ~mask) == 0 || (address & ~mask) == ~mask)
            FAIL("IPv4 address must not be the network or broadcast address");
        if (!web_parse_ipv4(s.ipv4_gateway, &gateway_be))
            FAIL("IPv4 gateway is not valid");
        const uint32_t gateway = ntohl(gateway_be);
        if ((gateway & mask) != (address & mask))
            FAIL("IPv4 gateway must be in the subnet of the address");
        if (gateway == address || (gateway & ~mask) == 0 || (gateway & ~mask) == ~mask)
            FAIL("IPv4 gateway must be another host address of the subnet");
    }

    // ---- IPv6 static address ----
    if (s.ipv6_address[0] != '\0')
    {
        uint8_t address[16];
        if (!web_parse_ipv6(s.ipv6_address, address))
            FAIL("IPv6 address is not valid");
        static constexpr uint8_t zero[16] = {};
        static constexpr uint8_t v4_mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (address[0] == 0xFF || (address[0] == 0xFE && (address[1] & 0xC0) == 0x80) ||
            memcmp(address, zero, 15) == 0 || memcmp(address, v4_mapped, 12) == 0)
            FAIL("IPv6 address: a global or unique local unicast address is required");
        // lwIP decides on-link with the first 64 bits, so shorter prefixes are not supported
        if (s.ipv6_prefix < 64 || s.ipv6_prefix > 128)
            FAIL("IPv6 prefix length: 64 to 128");
        if (s.ipv6_gateway[0] != '\0')
        {
            uint8_t gateway[16];
            if (!web_parse_ipv6(s.ipv6_gateway, gateway))
                FAIL("IPv6 gateway is not valid");
            const bool link_local = gateway[0] == 0xFE && (gateway[1] & 0xC0) == 0x80;
            const bool same_64 = memcmp(gateway, address, 8) == 0;
            if (gateway[0] == 0xFF || memcmp(gateway, zero, 16) == 0 || memcmp(gateway, address, 16) == 0 || (!link_local && !same_64))
                FAIL("IPv6 gateway must be a link-local address or in the /64 of the address");
        }
    }
    else if (s.ipv6_gateway[0] != '\0')
        FAIL("IPv6 gateway requires a static IPv6 address");
    else if (s.ipv6_prefix < 64 || s.ipv6_prefix > 128)
        FAIL("IPv6 prefix length: 64 to 128");

    // ---- DNS ----
    for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
    {
        if (s.dns[i][0] == '\0')
            continue;
        uint32_t v4 = 0;
        uint8_t v6[16];
        if (web_parse_ipv4(s.dns[i], &v4))
        {
            const uint8_t first = static_cast<uint8_t>(ntohl(v4) >> 24);
            if (first == 0 || first >= 224)
                FAIL("DNS server %u: not a unicast address", static_cast<unsigned>(i + 1));
        }
        else if (web_parse_ipv6(s.dns[i], v6))
        {
            if (v6[0] == 0xFF)
                FAIL("DNS server %u: not a unicast address", static_cast<unsigned>(i + 1));
        }
        else
            FAIL("DNS server %u is not an IPv4 or IPv6 address", static_cast<unsigned>(i + 1));
    }

    // ---- GPIOs ----
    struct pin_use_t
    {
        int pin;
        const char *name;
    };
    pin_use_t pins[8];
    size_t pin_count = 0;
    auto add_pin = [&](int pin, const char *name) -> bool
    {
        if (web_is_reserved_gpio(pin))
            return false;
        pins[pin_count++] = {pin, name};
        return true;
    };
    if (!add_pin(s.gnss_tx_pin, "GNSS TX"))
        FAIL("GNSS TX pin: GPIO %d cannot be used", s.gnss_tx_pin);
    if (!add_pin(s.gnss_rx_pin, "GNSS RX"))
        FAIL("GNSS RX pin: GPIO %d cannot be used", s.gnss_rx_pin);
    if (!add_pin(s.gnss_pps_pin, "GNSS PPS"))
        FAIL("PPS pin: GPIO %d cannot be used", s.gnss_pps_pin);
    if (s.ptp_loopback_pin != -1 && !add_pin(s.ptp_loopback_pin, "PTP PPS loopback"))
        FAIL("PTP PPS loopback pin: GPIO %d cannot be used", s.ptp_loopback_pin);
    switch (s.led_type)
    {
    case web_led_type_t::none:
        break;
    case web_led_type_t::ky016:
        if (!add_pin(s.led_red_pin, "LED red") || !add_pin(s.led_green_pin, "LED green") || !add_pin(s.led_blue_pin, "LED blue"))
            FAIL("KY-016 LED pins: one of the GPIOs cannot be used");
        break;
    case web_led_type_t::ws2812:
        if (!add_pin(s.led_ws2812_pin, "WS2812 data"))
            FAIL("WS2812 data pin: GPIO %d cannot be used", s.led_ws2812_pin);
        break;
    default:
        FAIL("LED type is not valid");
    }
    for (size_t i = 0; i < pin_count; ++i)
        for (size_t j = i + 1; j < pin_count; ++j)
            if (pins[i].pin == pins[j].pin)
                FAIL("GPIO %d is used twice (%s, %s)", pins[i].pin, pins[i].name, pins[j].name);

    static constexpr uint32_t bauds[] = {4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    bool baud_ok = false;
    for (uint32_t b : bauds)
        baud_ok = baud_ok || b == s.gnss_baud;
    if (!baud_ok)
        FAIL("GNSS baud rate is not one of the supported values");

    // ---- LCD and time zone ----
    {
        const size_t length = strnlen(s.lcd_title, WEB_LCD_TITLE_SIZE);
        if (length == 0 || length >= WEB_LCD_TITLE_SIZE || !is_printable_ascii(s.lcd_title, 0x20, 0x7D))
            FAIL("LCD title: 1 to 17 printable ASCII characters");
    }
    if (!web_is_valid_time_zone(s.time_zone))
        FAIL("Time zone is not a valid POSIX TZ string");
    if (s.led_brightness_percent > 100)
        FAIL("LED brightness: 0 to 100 %%");
    for (size_t i = 0; i < WEB_LED_STATE_COUNT; ++i)
        if (s.led_rgb[i] > 0xFFFFFFu)
            FAIL("LED colour %u is not a 24-bit RGB value", static_cast<unsigned>(i + 1));

    // ---- MQTT ----
    {
        const size_t length = strnlen(s.mqtt_uri, WEB_MQTT_URI_SIZE);
        if (length >= WEB_MQTT_URI_SIZE)
            FAIL("MQTT broker URI is too long");
        if (s.mqtt_enabled && length == 0)
            FAIL("MQTT broker URI is required when MQTT is on");
        if (length > 0)
        {
            const char *host = nullptr;
            if (strncmp(s.mqtt_uri, "mqtt://", 7) == 0)
                host = s.mqtt_uri + 7;
            else if (strncmp(s.mqtt_uri, "mqtts://", 8) == 0)
                host = s.mqtt_uri + 8;
            else
                FAIL("MQTT broker URI must start with mqtt:// or mqtts://");
            if (!is_printable_ascii(s.mqtt_uri, 0x21, 0x7E) || strpbrk(host, "@/?#\"'<>\\`") != nullptr || host[0] == '\0' || host[0] == ':')
                FAIL("MQTT broker URI: scheme://host[:port] without user, path or spaces");
            const char *port = host[0] == '[' ? strstr(host, "]:") : strrchr(host, ':');
            if (host[0] == '[' && strchr(host, ']') == nullptr)
                FAIL("MQTT broker URI: IPv6 address must be in brackets");
            if (port != nullptr)
            {
                port += host[0] == '[' ? 2 : 1;
                char *end = nullptr;
                const long value = strtol(port, &end, 10);
                if (end == port || *end != '\0' || value < 1 || value > 65535)
                    FAIL("MQTT broker URI: port 1 to 65535");
            }
        }
        if (strnlen(s.mqtt_username, WEB_MQTT_CREDENTIAL_SIZE) >= WEB_MQTT_CREDENTIAL_SIZE || !is_printable_ascii(s.mqtt_username, 0x20, 0x7E))
            FAIL("MQTT user name: at most 64 printable ASCII characters");
        if (strnlen(s.mqtt_password, WEB_MQTT_CREDENTIAL_SIZE) >= WEB_MQTT_CREDENTIAL_SIZE || !is_printable_ascii(s.mqtt_password, 0x20, 0x7E))
            FAIL("MQTT password: at most 64 printable ASCII characters");
    }

    // ---- NTP authentication ----
    if (s.ntp_key_count > (s.ntp_key_override ? WEB_NTP_MAX_KEYS : 0))
        FAIL("NTP keys: at most %u", static_cast<unsigned>(WEB_NTP_MAX_KEYS));
    if (s.ntp_key_override && s.ntp_auth_enabled && s.ntp_key_count == 0)
        FAIL("NTP authentication needs at least one key");
    for (size_t i = 0; i < s.ntp_key_count; ++i)
    {
        const web_ntp_key_t &key = s.ntp_keys[i];
        if (key.id < 1 || key.id > 65535)
            FAIL("NTP key %u: key ID 1 to 65535", static_cast<unsigned>(i + 1));
        for (size_t k = 0; k < i; ++k)
            if (s.ntp_keys[k].id == key.id)
                FAIL("NTP key ID %lu is used twice", static_cast<unsigned long>(key.id));
        if (strnlen(key.key, WEB_NTP_KEY_SIZE) != 32)
            FAIL("NTP key %u: exactly 32 characters", static_cast<unsigned>(i + 1));
        for (size_t c = 0; c < 32; ++c)
            if (!isalnum(static_cast<unsigned char>(key.key[c])))
                FAIL("NTP key %u: letters and digits only", static_cast<unsigned>(i + 1));
    }

    // ---- Over The Ethernet updates ----
    {
        const size_t length = strnlen(s.ote_device_name, WEB_OTE_NAME_SIZE);
        if (length == 0 || length >= WEB_OTE_NAME_SIZE || s.ote_device_name[0] == '-' || s.ote_device_name[length - 1] == '-')
            FAIL("OTE device name: 1 to 32 letters, digits or '-' (not at the start or end)");
        for (size_t i = 0; i < length; ++i)
            if (!isalnum(static_cast<unsigned char>(s.ote_device_name[i])) && s.ote_device_name[i] != '-')
                FAIL("OTE device name: only letters, digits and '-'");
        // UDP ports in use on the device: NTP 123, DHCP 68, mDNS 5353, httpd control socket 32768 / 32769
        if (s.ote_port < 1024 || s.ote_port == 5353 || s.ote_port == 32768 || s.ote_port == 32769)
            FAIL("OTE port: 1024 to 65535, not 5353, 32768 or 32769");
        const size_t password_length = strnlen(s.ote_password, WEB_OTE_PASSWORD_SIZE);
        if (password_length >= WEB_OTE_PASSWORD_SIZE || !is_printable_ascii(s.ote_password, 0x21, 0x7E))
            FAIL("OTE password: at most 64 printable ASCII characters without spaces");
        if (s.ote_enabled && password_length < 8)
            FAIL("OTE password: at least 8 characters when OTE updates are on");
    }

#undef FAIL
    return true;
}

bool web_settings_network_differs(const web_settings_t &a, const web_settings_t &b)
{
    if (a.ipv4_dhcp != b.ipv4_dhcp || a.ipv6_slaac != b.ipv6_slaac || strcmp(a.ipv6_address, b.ipv6_address) != 0 ||
        strcmp(a.ipv6_gateway, b.ipv6_gateway) != 0 || (a.ipv6_address[0] != '\0' && a.ipv6_prefix != b.ipv6_prefix))
        return true;
    if (!a.ipv4_dhcp && (strcmp(a.ipv4_address, b.ipv4_address) != 0 || a.ipv4_prefix != b.ipv4_prefix ||
                         strcmp(a.ipv4_gateway, b.ipv4_gateway) != 0))
        return true;
    for (size_t i = 0; i < WEB_DNS_SERVER_COUNT; ++i)
        if (strcmp(a.dns[i], b.dns[i]) != 0)
            return true;
    return false;
}

bool web_settings_restart_fields_differ(const web_settings_t &a, const web_settings_t &b)
{
    return strcmp(a.fqdn, b.fqdn) != 0 || a.gnss_ublox != b.gnss_ublox ||
           a.gnss_tx_pin != b.gnss_tx_pin || a.gnss_rx_pin != b.gnss_rx_pin || a.gnss_pps_pin != b.gnss_pps_pin ||
           a.gnss_baud != b.gnss_baud || a.ptp_loopback_pin != b.ptp_loopback_pin ||
           a.lcd_enabled != b.lcd_enabled ||
           a.led_type != b.led_type || a.led_red_pin != b.led_red_pin || a.led_green_pin != b.led_green_pin ||
           a.led_blue_pin != b.led_blue_pin || a.led_ws2812_pin != b.led_ws2812_pin ||
           a.mqtt_enabled != b.mqtt_enabled || strcmp(a.mqtt_uri, b.mqtt_uri) != 0 ||
           strcmp(a.mqtt_username, b.mqtt_username) != 0 ||
           !web_constant_time_equal(reinterpret_cast<const uint8_t *>(a.mqtt_password),
                                    reinterpret_cast<const uint8_t *>(b.mqtt_password), WEB_MQTT_CREDENTIAL_SIZE) ||
           a.ote_enabled != b.ote_enabled || strcmp(a.ote_device_name, b.ote_device_name) != 0 || a.ote_port != b.ote_port ||
           !web_constant_time_equal(reinterpret_cast<const uint8_t *>(a.ote_password),
                                    reinterpret_cast<const uint8_t *>(b.ote_password), WEB_OTE_PASSWORD_SIZE);
}

bool web_settings_equal(const web_settings_t &a, const web_settings_t &b)
{
    if (web_settings_network_differs(a, b) || web_settings_restart_fields_differ(a, b))
        return false;
    return strcmp(a.lcd_title, b.lcd_title) == 0 && a.lcd_24_hour == b.lcd_24_hour && a.lcd_pad_hour == b.lcd_pad_hour &&
           a.lcd_show_time_zone == b.lcd_show_time_zone && strcmp(a.time_zone, b.time_zone) == 0 &&
           a.led_brightness_percent == b.led_brightness_percent && memcmp(a.led_rgb, b.led_rgb, sizeof(a.led_rgb)) == 0 &&
           a.ntp_auth_enabled == b.ntp_auth_enabled &&
           a.ntp_key_override == b.ntp_key_override && a.ntp_key_count == b.ntp_key_count &&
           web_constant_time_equal(reinterpret_cast<const uint8_t *>(a.ntp_keys), reinterpret_cast<const uint8_t *>(b.ntp_keys), sizeof(a.ntp_keys));
}
